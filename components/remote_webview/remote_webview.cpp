#include "remote_webview.h"
#include "remote_webview_config.h"
#include "esphome/core/log.h"

#include "esp_idf_version.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_websocket_client.h"
#include "esp_efuse.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <new>

namespace esphome {
namespace remote_webview {

static const char *const TAG = "Remote_WebView";
RemoteWebView *RemoteWebView::self_ = nullptr;

//Below two functions are part of automation template testing
void RemoteWebView::add_on_frame_update_callback(std::function<void()> &&callback) {
  this->on_frame_update_callback_.add(std::move(callback));
}
void RemoteWebView::trigger_on_frame_update() {
  uint32_t now = millis();
  
  if (now - this->last_trigger_ms_ < 1000) return;
  
  this->last_trigger_ms_ = now;
  
  ESP_LOGD(TAG, "Triggering the on_frame_update automation");
  this->on_frame_update_callback_.call();
}

void RemoteWebView::process_current_url_packet_(const uint8_t *data, size_t len) {
  if (!data || len < sizeof(proto::CurrentURLHeader)) return;
  
  auto *hdr = reinterpret_cast<const proto::CurrentURLHeader *>(data);
  if (sizeof(proto::CurrentURLHeader) + hdr->url_len > len) return;
  
  if (this->url_sensor_ == nullptr) return;

  std::string url(reinterpret_cast<const char*>(data + sizeof(proto::CurrentURLHeader)), hdr->url_len);
  if (this->state_mtx_ && xSemaphoreTake(this->state_mtx_, pdMS_TO_TICKS(10)) == pdTRUE) {
    this->pending_url_ = std::move(url);
    this->url_publish_pending_.store(true, std::memory_order_release);
    xSemaphoreGive(this->state_mtx_);
  }
}

std::string RemoteWebView::get_current_url() const {
  if (this->url_sensor_ != nullptr && this->url_sensor_->has_state()) {
    return this->url_sensor_->state;
  }
  return "";
}

static inline void websocket_force_reconnect(esp_websocket_client_handle_t client) {
  if (!client) return;
  esp_websocket_client_stop(client);
  esp_websocket_client_start(client);
}

void RemoteWebView::setup() {
  self_ = this;

  if (!display_) {
    ESP_LOGE(TAG, "no display");
    return;
  }

  // Fill the frame buffer black immediately so the panel shows a clean dark
  // screen instead of uninitialized PSRAM garbage (white/noise) while we
  // wait for WiFi + WebSocket to connect and the first real frame to arrive.
  display_->fill(esphome::Color(0, 0, 0));

  display_width_ = display_->get_width();
  display_height_ = display_->get_height();

  // Fixed pool of reassembly buffers, sized to the decode queue depth, so the
  // WS event handler and decode task never call malloc/free on the hot path
  // (see WsMsg/WsReasm comment in the header for why).
  reasm_pool_cap_ = (size_t)((max_bytes_per_msg_ > 0) ? max_bytes_per_msg_ : cfg::ws_max_message_bytes);
  q_free_ = xQueueCreate(kReasmPoolSize, sizeof(int));
  q_decode_ = xQueueCreate(cfg::decode_queue_depth, sizeof(WsMsg));
  ws_send_mtx_ = xSemaphoreCreateMutex();
  state_mtx_ = xSemaphoreCreateMutex();

  for (int i = 0; i < kReasmPoolSize; i++) {
    reasm_pool_[i] = (uint8_t *)heap_caps_malloc(reasm_pool_cap_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!reasm_pool_[i]) reasm_pool_[i] = (uint8_t *)heap_caps_malloc(reasm_pool_cap_, MALLOC_CAP_8BIT);
    if (!reasm_pool_[i]) {
      ESP_LOGE(TAG, "Failed to allocate reassembly buffer %d/%d (%u bytes)", i + 1, kReasmPoolSize, (unsigned)reasm_pool_cap_);
      continue;
    }
    xQueueSend(q_free_, &i, 0);
  }

  draw_mtx_ = xSemaphoreCreateMutex();
  stats_mtx_ = xSemaphoreCreateMutex();

  // Decide the worker count with staged fallbacks: dual-core can be disabled
  // from YAML, is never used with the P4 hardware decoder (hw_dec_ and its
  // buffers are shared, not thread-safe), and degrades back to one worker if
  // the second decoder's memory or task can't be allocated.
#if REMOTE_WEBVIEW_HW_JPEG
  int want_workers = 1;
#else
  int want_workers = dual_core_decode_ ? kMaxDecodeWorkers : 1;
#endif

  for (int w = 0; w < want_workers; w++) {
    JPEGDEC *jd = alloc_jpegdec_();
    if (!jd) {
      if (w == 0) {
        ESP_LOGE(TAG, "Failed to allocate JPEGDEC state (%u bytes)", (unsigned) sizeof(JPEGDEC));
      } else {
        ESP_LOGW(TAG, "No memory for decoder %d, staying single-core", w);
      }
      want_workers = w;
      break;
    }
    workers_[w] = DecodeWorker{this, jd, w};
  }
  decode_workers_ = want_workers;

  start_decode_tasks_();
  start_ws_task_();

  ESP_LOGI(TAG, "setup done, free heap: internal=%u psram=%u",
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  if (touch_) {
    touch_listener_ = new RemoteWebViewTouchListener(this);
    touch_->register_listener(touch_listener_);
    ESP_LOGD(TAG, "touch listener registered");
  }

#if REMOTE_WEBVIEW_HW_JPEG
  jpeg_decode_engine_cfg_t jcfg = {
    .timeout_ms = 200,
  };
  if (jpeg_new_decoder_engine(&jcfg, &hw_dec_) != ESP_OK) {
    hw_dec_ = nullptr;
  }
  
  if (hw_dec_) {
    const int W = display_->get_width();
    const int H = display_->get_height();
    const int aligned_w = (W + 15) & ~15;
    const int aligned_h = (H + 15) & ~15;
    
    const size_t max_buffer_size = (size_t)aligned_w * (size_t)aligned_h * 2u;
    
    jpeg_decode_memory_alloc_cfg_t in_cfg { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
    jpeg_decode_memory_alloc_cfg_t out_cfg { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
    
    hw_decode_input_buf_ = (uint8_t*)jpeg_alloc_decoder_mem((uint32_t)max_buffer_size, &in_cfg, &hw_decode_input_size_);
    hw_decode_output_buf_ = (uint8_t*)jpeg_alloc_decoder_mem((uint32_t)max_buffer_size, &out_cfg, &hw_decode_output_size_);
    
    if (!hw_decode_input_buf_ || !hw_decode_output_buf_) {
      ESP_LOGE(TAG, "Failed to allocate HW decoder buffers");
      if (hw_decode_input_buf_) free(hw_decode_input_buf_);
      if (hw_decode_output_buf_) free(hw_decode_output_buf_);
      hw_decode_input_buf_ = nullptr;
      hw_decode_output_buf_ = nullptr;
      jpeg_del_decoder_engine(hw_dec_);
      hw_dec_ = nullptr;
    } else {
      ESP_LOGD(TAG, "HW decoder buffers allocated: input=%u, output=%u", 
               (unsigned)hw_decode_input_size_, (unsigned)hw_decode_output_size_);
    }
  }
#endif
}

void RemoteWebView::loop() {
  if (this->frame_update_pending_.exchange(false, std::memory_order_acq_rel)) {
    this->trigger_on_frame_update();
  }

  if (!this->url_sensor_) return;
  if (!this->url_publish_pending_.exchange(false, std::memory_order_acq_rel)) return;

  std::string url;
  if (this->state_mtx_ && xSemaphoreTake(this->state_mtx_, pdMS_TO_TICKS(10)) == pdTRUE) {
    url = this->pending_url_;
    xSemaphoreGive(this->state_mtx_);
  }

  if (!url.empty()) {
    this->url_sensor_->publish_state(url);
    ESP_LOGD(TAG, "Current Server URL updated: %s", url.c_str());
  }
}

void RemoteWebView::dump_config() {
  ESP_LOGCONFIG(TAG, "remote_webview:");
  ESP_LOGCONFIG(TAG, "  version: %s", cfg::component_version);
  ESP_LOGCONFIG(TAG, "  decode workers: %d", decode_workers_);

  const std::string id = device_id_.empty() ? resolve_device_id_() : device_id_;
  ESP_LOGCONFIG(TAG, "  id: %s", id.c_str());

  if (display_) {
    ESP_LOGCONFIG(TAG, "  display: %dx%d", display_->get_width(), display_->get_height());
  }

#if REMOTE_WEBVIEW_HW_JPEG
  ESP_LOGCONFIG(TAG, "  hw_jpeg: %s", hw_dec_ ? "yes" : "no");
#else
  ESP_LOGCONFIG(TAG, "  hw_jpeg: no");
#endif

  ESP_LOGCONFIG(TAG, "  server: %s:%d", server_host_.c_str(), server_port_);
  ESP_LOGCONFIG(TAG, "  url: %s", url_.c_str());

  auto print_opt_int = [&](const char *name, int v) {
    if (v >= 0) ESP_LOGCONFIG(TAG, "  %s: %d", name, v);
  };
  auto print_opt_float2 = [&](const char *name, float v) {
    if (v >= 0.0f) ESP_LOGCONFIG(TAG, "  %s: %.2f", name, (double)v);
  };

  print_opt_int   ("tile_size",                 tile_size_);
  print_opt_int   ("full_frame_tile_count",     full_frame_tile_count_);
  print_opt_float2("full_frame_area_threshold", full_frame_area_threshold_);
  print_opt_int   ("full_frame_every",          full_frame_every_);
  print_opt_int   ("every_nth_frame",           every_nth_frame_);
  print_opt_int   ("min_frame_interval",        min_frame_interval_);
  print_opt_int   ("jpeg_quality",              jpeg_quality_);
  print_opt_int   ("max_bytes_per_msg",         max_bytes_per_msg_);
  print_opt_int   ("big_endian",                rgb565_big_endian_);
  print_opt_int   ("rotation",                  rotation_);
}

bool RemoteWebView::open_url(const std::string &s) {
  if (s.empty()) return false;
  
  if (!ws_client_ || !esp_websocket_client_is_connected(ws_client_))
    return false;
  
  if (ws_send_open_url_(s.c_str(), 0)) {
    url_ = s;
    ESP_LOGD(TAG, "opened URL: %s", s.c_str());
    return true;
  }
  
  return false;
}

void RemoteWebView::start_ws_task_() {
  xTaskCreatePinnedToCore(&RemoteWebView::ws_task_tramp_, "rwv_ws", cfg::ws_task_stack, this, 5, &t_ws_, 0);
}

void RemoteWebView::ws_task_tramp_(void *arg) {
  auto *self = reinterpret_cast<RemoteWebView*>(arg);

  // Wait until the default network interface has a valid (non-zero) IP address
  // before attempting a WebSocket connection. Without this, the task tries to
  // connect while WiFi is still associating, burning retry cycles and logging
  // spurious "Host is unreachable" errors that mask real failures later.
  {
    ESP_LOGI(TAG, "[ws] waiting for network IP...");
    for (;;) {
      esp_netif_t *sta = esp_netif_get_default_netif();
      if (sta) {
        esp_netif_ip_info_t ip{};
        if (esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) break;
      }
      vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "[ws] network ready, starting WebSocket");
  }

  std::string uri_str = self->build_ws_uri_();
  esp_websocket_client_config_t cfg_ws = {};
  cfg_ws.uri = uri_str.c_str();
  cfg_ws.reconnect_timeout_ms = 2000;
  cfg_ws.network_timeout_ms   = 10000;
  cfg_ws.task_stack           = cfg::ws_task_stack;
  cfg_ws.task_prio            = cfg::ws_task_prio;
  cfg_ws.buffer_size          = cfg::ws_buffer_size;
  cfg_ws.disable_auto_reconnect = false;

  // Never ESP_ERROR_CHECK (i.e. abort) on client setup: init allocates the
  // client and its ~2x30KB rx/tx buffers from internal SRAM, and a failure
  // here is almost always transient memory pressure right after WiFi comes
  // up. Aborting turned that into a reboot loop (each boot hit the same
  // pressure at the same moment); retrying quietly succeeds a few seconds
  // later once startup allocations settle.
  WsReasm reasm{};
  esp_websocket_client_handle_t client = nullptr;
  for (;;) {
    client = esp_websocket_client_init(&cfg_ws);
    if (client) {
      const esp_err_t err = esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, ws_event_handler_, &reasm);
      if (err == ESP_OK) break;
      ESP_LOGE(TAG, "[ws] register_events failed: 0x%x", (unsigned) err);
      esp_websocket_client_destroy(client);
      client = nullptr;
    } else {
      ESP_LOGE(TAG, "[ws] client init failed (free internal heap: %u bytes) — retrying",
               (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
  while (esp_websocket_client_start(client) != ESP_OK) {
    ESP_LOGE(TAG, "[ws] client start failed — retrying");
    vTaskDelay(pdMS_TO_TICKS(3000));
  }

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(5000));

    if (!esp_websocket_client_is_connected(client)) {
      websocket_force_reconnect(client);
      continue;
    }

    if (self && self->ws_client_ && esp_websocket_client_is_connected(self->ws_client_)) {
      const uint64_t now = esp_timer_get_time();
      if (now - self->last_keepalive_us_ >= cfg::ws_keepalive_interval_us) {
        if (self->ws_send_keepalive_()) {
          self->last_keepalive_us_ = now;
          ESP_LOGV(TAG, "[ws] keepalive sent");
        }
      }
    }
  }
}

void RemoteWebView::reasm_release_(RemoteWebView *self, WsReasm &r) {
  if (r.slot >= 0 && self && self->q_free_) {
    int slot = r.slot;
    xQueueSend(self->q_free_, &slot, 0);
  }
  r.slot = -1; r.total = 0; r.filled = 0;
}

void RemoteWebView::ws_event_handler_(void *handler_arg, esp_event_base_t, int32_t event_id, void *event_data) {
  auto *r = reinterpret_cast<WsReasm*>(handler_arg);
  auto *e = reinterpret_cast<const esp_websocket_event_data_t *>(event_data);

  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      if (self_) self_->ws_client_ = e->client;
      ESP_LOGI(TAG, "[ws] connected");
      
      if (self_) self_->last_keepalive_us_ = esp_timer_get_time();
      if (self_ && !self_->url_.empty()) {
        self_->ws_send_open_url_(self_->url_.c_str(), 0);
      }
      break;

    case WEBSOCKET_EVENT_DISCONNECTED:
      if (self_) self_->ws_client_ = nullptr;
      ESP_LOGI(TAG, "[ws] disconnected");
      if (self_) self_->last_keepalive_us_ = 0;
      reasm_release_(self_, *r);
      websocket_force_reconnect(e->client);
      break;

#ifdef WEBSOCKET_EVENT_CLOSED
    case WEBSOCKET_EVENT_CLOSED:
      if (self_) self_->ws_client_ = nullptr;
      ESP_LOGI(TAG, "[ws] closed");
      if (self_) self_->last_keepalive_us_ = 0;
      reasm_release_(self_, *r);
      websocket_force_reconnect(e->client);
      break;
#endif

    case WEBSOCKET_EVENT_DATA: {
      if (!self_) break;

      const uint8_t *frag = (const uint8_t *)e->data_ptr;
      size_t frag_len = (size_t)e->data_len;
      bool is_bin  = (e->op_code == WS_TRANSPORT_OPCODES_BINARY);
      if (!is_bin) break;

      if (e->payload_offset == 0) {
        reasm_release_(self_, *r);
        const size_t max_allowed = self_->reasm_pool_cap_;
        if ((size_t)e->payload_len > max_allowed) {
          ESP_LOGE(TAG, "WS message too large: %u > %u", (unsigned)e->payload_len, (unsigned)max_allowed);
          break;
        }
        int slot;
        if (!self_->q_free_ || xQueueReceive(self_->q_free_, &slot, 0) != pdTRUE) {
          ESP_LOGW(TAG, "reassembly pool exhausted, dropping message");
          break;
        }
        r->slot  = slot;
        r->total = (size_t)e->payload_len;
      }
      if (r->slot < 0 || r->total == 0) break;

      if ((size_t)e->payload_offset + frag_len > r->total) {
        ESP_LOGE(TAG, "bad fragment bounds");
        reasm_release_(self_, *r);
        break;
      }
      memcpy(self_->reasm_pool_[r->slot] + e->payload_offset, frag, frag_len);
      size_t new_filled = (size_t)e->payload_offset + frag_len;
      if (new_filled > r->filled) r->filled = new_filled;

      if (r->filled == r->total) {
        WsMsg m;
        m.slot = r->slot; m.len = r->total;
        m.t_enq_us = esp_timer_get_time();
        r->slot = -1; r->total = 0; r->filled = 0;
        if (!self_->q_decode_ || xQueueSend(self_->q_decode_, &m, 0) != pdTRUE) {
          ESP_LOGW(TAG, "decode queue full, dropping packet");
          xQueueSend(self_->q_free_, &m.slot, 0);
        }
      }
      break;
    }

    case WEBSOCKET_EVENT_ERROR:
      ESP_LOGE(TAG, "[ws] error: type=%d tls_err=%d tls_stack=%d",
               e->error_handle.error_type,
               e->error_handle.esp_tls_last_esp_err,
               e->error_handle.esp_tls_stack_err);
      break;

    default: break;
  }
}

JPEGDEC *RemoteWebView::alloc_jpegdec_() {
  // Internal SRAM first: the decoder touches its ~25-40 KB working set for
  // every 8x8 block, and as a plain member it would land in PSRAM (the
  // component object exceeds ESP-IDF's "always internal" malloc threshold).
  auto *jd = (JPEGDEC *) heap_caps_malloc(sizeof(JPEGDEC), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!jd) jd = (JPEGDEC *) heap_caps_malloc(sizeof(JPEGDEC), MALLOC_CAP_8BIT);
  if (!jd) return nullptr;
  new (jd) JPEGDEC();  // value-init zeroes the decoder state
  ESP_LOGI(TAG, "JPEGDEC state (%u bytes) in %s RAM", (unsigned) sizeof(JPEGDEC),
           esp_ptr_internal(jd) ? "internal" : "PSRAM");
  return jd;
}

void RemoteWebView::start_decode_tasks_() {
  for (int w = 0; w < decode_workers_; w++) {
    const int core = (w == 0) ? 1 : 0;
    const int prio = (w == 0) ? cfg::decode_task_prio_core1 : cfg::decode_task_prio_core0;
    const char *name = (w == 0) ? "rwv_dec0" : "rwv_dec1";
    if (xTaskCreatePinnedToCore(&RemoteWebView::decode_task_tramp_, name, cfg::decode_task_stack,
                                &workers_[w], prio, &t_decode_[w], core) != pdPASS) {
      ESP_LOGW(TAG, "Failed to start decode worker %d%s", w,
               (w > 0) ? ", staying single-core" : "");
      decode_workers_ = w;
      break;
    }
  }
  ESP_LOGI(TAG, "decode workers: %d", decode_workers_);
}

void RemoteWebView::decode_task_tramp_(void *arg) {
  auto *ctx = reinterpret_cast<DecodeWorker*>(arg);
  auto *self = ctx->rwv;
  WsMsg m;
  uint32_t streak = 0;
  for (;;) {
    if (xQueueReceive(self->q_decode_, &m, portMAX_DELAY) != pdTRUE) continue;
    self->process_packet_(ctx->jd, self->reasm_pool_[m.slot], m.len, m.t_enq_us);
    xQueueSend(self->q_free_, &m.slot, portMAX_DELAY);

    // Watchdog protection: during sustained animation the queue can stay
    // non-empty indefinitely, so this task would never block and the idle
    // task — which the ESP-IDF Task Watchdog (5s) monitors — starves,
    // panicking the device. But an unconditional vTaskDelay(1) per message
    // costs up to a full tick each, which is real money for 1-2 ms partial
    // tiles at 30fps. So: when the queue drains, the blocking receive above
    // yields naturally and no delay is needed; only during an unbroken backlog
    // hand idle a slice every few messages (worst case well under 200 ms
    // between slices — far inside the 5 s watchdog window).
    if (uxQueueMessagesWaiting(self->q_decode_) == 0) {
      streak = 0;
    } else if (++streak >= cfg::decode_yield_every) {
      streak = 0;
      vTaskDelay(1);
    }
  }
}

void RemoteWebView::process_packet_(JPEGDEC *jd, const uint8_t *data, size_t len, int64_t t_enq_us) {
  if (!data || len == 0) return;

  const proto::MsgType type = (proto::MsgType)data[0];
  switch (type) {
    case proto::MsgType::Frame:
      process_frame_packet_(jd, data, len, t_enq_us);
      break;
    case proto::MsgType::FrameStats:
      process_frame_stats_packet_(data, len);
      break;
    case proto::MsgType::CurrentURL: // deal with packet #6 here - aka our current display URL packet
      process_current_url_packet_(data, len);
      break;
    default:
      ESP_LOGW(TAG, "unknown packet type: %d", (int)type);
      break;
  }
}

// Admit this message into the current frame, or — if it belongs to a newer
// frame — wait until every in-flight decode of the previous frame finishes.
// The decode queue is FIFO, so a worker holding frame N+1 implies all frame-N
// messages were already dequeued; draining inflight is therefore a complete
// ordering guarantee. Also owns the per-frame stats reset/accumulation that
// used to live unguarded in process_frame_packet_.
//
// Returns false when the message is for a frame OLDER than one that already
// started (serial-number comparison). That happens only in a narrow race: a
// worker dequeues frame N+1, gets preempted before reaching this barrier, and
// the other worker meanwhile dequeues N+2 and advances past it. Drawing the
// stale N+1 tiles would overwrite newer pixels; dropping them merely leaves
// that region one frame behind until the next update or drift sweep.
bool RemoteWebView::frame_barrier_enter_(uint32_t frame_id, size_t msg_len, uint16_t tile_count, int64_t t_enq_us) {
  for (;;) {
    xSemaphoreTake(stats_mtx_, portMAX_DELAY);
    if (frame_id == frame_id_) {
      frame_bytes_ += msg_len;
      frame_tiles_ += tile_count;
      if (t_enq_us < frame_first_enq_us_) frame_first_enq_us_ = t_enq_us;
      if (t_enq_us > frame_last_enq_us_) frame_last_enq_us_ = t_enq_us;
      barrier_inflight_++;
      xSemaphoreGive(stats_mtx_);
      return true;
    }
    if (barrier_inflight_ == 0) {
      if (frame_id_ != 0xffffffffu && (int32_t)(frame_id - frame_id_) < 0) {
        xSemaphoreGive(stats_mtx_);
        ESP_LOGW(TAG, "dropping stale frame %lu (current %lu)", (unsigned long)frame_id, (unsigned long)frame_id_);
        return false;
      }
      frame_id_ = frame_id;
      frame_tiles_ = tile_count;
      frame_bytes_ = msg_len;
      frame_start_us_ = esp_timer_get_time();
      frame_first_enq_us_ = t_enq_us;
      frame_last_enq_us_ = t_enq_us;
      frame_decode_us_.store(0, std::memory_order_relaxed);
      frame_draw_us_.store(0, std::memory_order_relaxed);
      barrier_inflight_++;
      xSemaphoreGive(stats_mtx_);
      return true;
    }
    xSemaphoreGive(stats_mtx_);
    vTaskDelay(1);  // previous frame still decoding on the other worker
  }
}

void RemoteWebView::frame_barrier_exit_() {
  xSemaphoreTake(stats_mtx_, portMAX_DELAY);
  barrier_inflight_--;
  xSemaphoreGive(stats_mtx_);
}

void RemoteWebView::process_frame_packet_(JPEGDEC *jd, const uint8_t *data, size_t len, int64_t t_enq_us)
{
  if (!data || len < sizeof(proto::FrameHeader)) return;

  proto::FrameInfo fi{};
  size_t off = 0;
  if (!proto::parse_frame_header(data, len, fi, off)) return;

  if (!frame_barrier_enter_(fi.frame_id, len, fi.tile_count, t_enq_us))
    return;  // stale frame dropped — no inflight slot was taken

  for (uint16_t i = 0; i < fi.tile_count; i++) {
    proto::TileHeader th{};
    if (!proto::parse_tile_header(data, len, th, off)) break;
    // th.dlen is attacker/corruption-controlled (uint32_t from the wire); computing
    // off + th.dlen can wrap around size_t and pass an overflowed check, letting a
    // decode call read far past the end of `data`. Compare the other way instead,
    // which parse_tile_header already guarantees can't underflow (off <= len).
    if (th.dlen > len - off) break;

    if (th.w == 0 || th.h == 0 || th.w > display_width_ || th.h > display_height_) {
      off += th.dlen;
      continue;
    }

    if (fi.enc == proto::Encoding::JPEG && th.dlen) {
      decode_jpeg_tile_to_lcd_(jd, (int16_t)th.x, (int16_t)th.y, data + off, th.dlen);
    }

    off += th.dlen;
  }

  if (fi.flags & proto::kFlafLastOfFrame) {
    // Note: with two workers a sibling message of this frame may still be
    // decoding when the flagged message finishes, so the recorded time can
    // read slightly short. It feeds only the debug log, self-test averages,
    // and the (once-per-second-throttled) on_frame_update trigger — none of
    // which warrant a completion-tracking mechanism.
    xSemaphoreTake(stats_mtx_, portMAX_DELAY);
    const uint32_t time_ms = (esp_timer_get_time() - frame_start_us_) / 1000ULL;
    frame_stats_bytes_ += frame_bytes_;
    frame_stats_time_ += time_ms;
    frame_stats_count_++;
    ESP_LOGD(TAG, "frame %lu: tiles %u (%u bytes) - %lu ms", frame_id_, frame_tiles_, frame_bytes_, time_ms);

    // Heavy-frame diagnostic (rate-limited): splits a slow frame's wall time
    // into waiting-for-arrival vs decode vs draw so the bottleneck is
    // measurable instead of guessed. arrive = spread between the first and
    // last message of the frame reaching the decode queue (network/radio
    // pacing); decode includes draw (drawing happens inside the decoder's
    // callbacks); draw includes draw-mutex wait.
    const uint32_t arrive_ms = (uint32_t)((frame_last_enq_us_ - frame_first_enq_us_) / 1000);
    const uint32_t decode_ms = frame_decode_us_.load(std::memory_order_relaxed) / 1000;
    const uint32_t draw_ms   = frame_draw_us_.load(std::memory_order_relaxed) / 1000;
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (time_ms >= 25 && (now_ms - last_heavy_log_ms_) >= 1000) {
      last_heavy_log_ms_ = now_ms;
      ESP_LOGI(TAG, "heavy frame %lu: wall=%lums arrive=%lums decode=%lums (draw=%lums) tiles=%u bytes=%u",
               (unsigned long)frame_id_, (unsigned long)time_ms, (unsigned long)arrive_ms,
               (unsigned long)decode_ms, (unsigned long)draw_ms, frame_tiles_, (unsigned)frame_bytes_);
    }
    xSemaphoreGive(stats_mtx_);

    this->frame_update_pending_.store(true, std::memory_order_release);
  }

  frame_barrier_exit_();
}

void RemoteWebView::process_frame_stats_packet_(const uint8_t *data, size_t len)
{
  xSemaphoreTake(stats_mtx_, portMAX_DELAY);
  uint32_t avg_render_time = 0;
  if (frame_stats_count_ > 0)
    avg_render_time = frame_stats_time_ / frame_stats_count_;
  const size_t stats_bytes = frame_stats_bytes_;

  frame_stats_time_ = 0;
  frame_stats_count_ = 0;
  frame_stats_bytes_ = 0;
  xSemaphoreGive(stats_mtx_);

  ESP_LOGD(TAG, "sending frame stats: avg_time=%u ms, bytes=%u", (unsigned)avg_render_time, (unsigned)stats_bytes);
  uint8_t pkt[sizeof(proto::FrameStatsPacket)];
  const size_t n = proto::build_frame_stats_packet(avg_render_time, stats_bytes, pkt);

  const TickType_t to = pdMS_TO_TICKS(50);
  if (xSemaphoreTake(ws_send_mtx_, to) != pdTRUE)
    return;

  esp_websocket_client_send_bin(ws_client_, (const char*)pkt, (int)n, to);
  xSemaphoreGive(ws_send_mtx_);
}

bool RemoteWebView::decode_jpeg_tile_to_lcd_(JPEGDEC *jd, int16_t dst_x, int16_t dst_y, const uint8_t *data, size_t len) {
  if (!data || !len) return false;

#if REMOTE_WEBVIEW_HW_JPEG
  // Note: the HW decoder and its buffers are shared, which is why setup()
  // caps decode_workers_ at 1 when REMOTE_WEBVIEW_HW_JPEG is enabled.
  if (hw_dec_ && hw_decode_input_buf_ && hw_decode_output_buf_) {
    jpeg_decode_picture_info_t hdr{};
    if (jpeg_decoder_get_info(data, (uint32_t)len, &hdr) != ESP_OK || !hdr.width || !hdr.height) {
      return decode_jpeg_tile_software_(jd, dst_x, dst_y, data, len);
    }

    const int aligned_w = (hdr.width  + 15) & ~15;
    const int aligned_h = (hdr.height + 15) & ~15;
    const uint32_t out_sz = (uint32_t)aligned_w * (uint32_t)aligned_h * 2u;

    if (aligned_w != (int)hdr.width) {
      ESP_LOGW(TAG, "jpeg dimensions not aligned: %u x %u", (unsigned)hdr.width, (unsigned)hdr.height);
      return decode_jpeg_tile_software_(jd, dst_x, dst_y, data, len);
    }

    if (len > hw_decode_input_size_ || out_sz > hw_decode_output_size_) {
      ESP_LOGW(TAG, "tile too large for HW decoder buffers");
      return decode_jpeg_tile_software_(jd, dst_x, dst_y, data, len);
    }

    jpeg_decode_cfg_t jcfg{};
    jcfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    jcfg.rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
    jcfg.conv_std      = JPEG_YUV_RGB_CONV_STD_BT709;

    memcpy(hw_decode_input_buf_, data, len);

    uint32_t written = 0;
    esp_err_t dr = jpeg_decoder_process(hw_dec_, &jcfg, hw_decode_input_buf_, (uint32_t)len,
                                        hw_decode_output_buf_, (uint32_t)hw_decode_output_size_, &written);

    if (dr != ESP_OK) {
      return decode_jpeg_tile_software_(jd, dst_x, dst_y, data, len);
    }

    xSemaphoreTake(draw_mtx_, portMAX_DELAY);
    display_->draw_pixels_at(dst_x, dst_y, (int)hdr.width, (int)hdr.height, hw_decode_output_buf_,
        esphome::display::COLOR_ORDER_RGB,
        esphome::display::COLOR_BITNESS_565,
        rgb565_big_endian_);
    xSemaphoreGive(draw_mtx_);

    return true;
  }
#endif  // REMOTE_WEBVIEW_HW_JPEG

  return decode_jpeg_tile_software_(jd, dst_x, dst_y, data, len);
}

bool RemoteWebView::decode_jpeg_tile_software_(JPEGDEC *jd, int16_t dst_x, int16_t dst_y, const uint8_t *data, size_t len) {
  if (!jd) return false;

  if (!jd->openRAM((uint8_t*)data, (int)len, &RemoteWebView::jpeg_draw_cb_s_)) {
    ESP_LOGE(TAG, "openRAM failed (len=%u) err=%d", (unsigned)len, jd->getLastError());
    return false;
  }

  jd->setMaxOutputSize(8 * 2048);
  jd->setPixelType(rgb565_big_endian_ ? RGB565_BIG_ENDIAN : RGB565_LITTLE_ENDIAN);

  const int64_t t0 = esp_timer_get_time();
  const int rc = jd->decode(dst_x, dst_y, 0);
  // Includes time spent in draw callbacks (drawing happens inside decode);
  // the heavy-frame log reports draw separately so the two can be split.
  frame_decode_us_.fetch_add((uint32_t)(esp_timer_get_time() - t0), std::memory_order_relaxed);
  if (rc == 0) {
    ESP_LOGE(TAG, "decode rc=%d err=%d", rc, jd->getLastError());
    jd->close();
    return false;
  }
  jd->close();
  return true;
}

int RemoteWebView::jpeg_draw_cb_s_(JPEGDRAW *p) {
  return self_ ? self_->jpeg_draw_cb_(p) : 0;
}

// Runs inside JPEGDEC's decode loop on whichever worker owns the decode.
// Reads only immutable-after-setup state (display_, bounds, endianness);
// the draw itself is serialized across workers via draw_mtx_ because
// esp_lcd's memcpy+cache-writeback path is not documented safe for
// concurrent cross-core calls. Serializing draw costs almost nothing:
// the PSRAM bus is a single shared resource, so parallel draws would
// contend head-to-head anyway — the parallel win lives in the decode
// (Huffman + SIMD) that happens between these callbacks.
int RemoteWebView::jpeg_draw_cb_(JPEGDRAW *p) {
  int32_t x = p->x, y = p->y, w = p->iWidth, h = p->iHeight;

  if (x >= display_width_ || y >= display_height_) return 1;
  if (x + w > display_width_) w = display_width_ - x;
  if (y + h > display_height_) h = display_height_ - y;
  if (w <= 0 || h <= 0) return 1;

  const int64_t t0 = esp_timer_get_time();
  xSemaphoreTake(draw_mtx_, portMAX_DELAY);
  display_->draw_pixels_at(
      x, y, w, h,
      (const uint8_t *)p->pPixels,
      esphome::display::COLOR_ORDER_RGB,
      esphome::display::COLOR_BITNESS_565,
      rgb565_big_endian_
  );
  xSemaphoreGive(draw_mtx_);
  // Includes any wait on draw_mtx_, so cross-worker draw contention shows
  // up here rather than hiding in the decode number.
  frame_draw_us_.fetch_add((uint32_t)(esp_timer_get_time() - t0), std::memory_order_relaxed);

  return 1;
}

bool RemoteWebView::ws_send_touch_event_(proto::TouchType type, int x, int y, uint8_t pid) {
  if (touch_disabled_)
    return false;

  if (!ws_client_ || !ws_send_mtx_ || !esp_websocket_client_is_connected(ws_client_))
    return false;

  if (x < 0) x = 0; if (y < 0) y = 0;
  if (x > 65535) x = 65535; if (y > 65535) y = 65535;

  uint8_t pkt[sizeof(proto::TouchPacket)];
  const size_t n = proto::build_touch_packet(type, pid, x, y, pkt);

  const TickType_t to = pdMS_TO_TICKS(50);
  if (xSemaphoreTake(ws_send_mtx_, pdMS_TO_TICKS(10)) != pdTRUE)
    return false;

  int r = esp_websocket_client_send_bin(ws_client_, (const char*)pkt, (int)n, to);
  xSemaphoreGive(ws_send_mtx_);
  return r == (int)n;
}

bool RemoteWebView::ws_send_open_url_(const char *url, uint16_t flags) {
  if (!ws_client_ || !ws_send_mtx_ ||  !url || !esp_websocket_client_is_connected(ws_client_))
    return false;

  const uint32_t n = (uint32_t) strlen(url);
  const size_t total = sizeof(proto::OpenURLHeader) + (size_t) n;
  
  if (total > 16 * 1024) return false;

    auto *pkt = (uint8_t *) heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!pkt) pkt = (uint8_t *) heap_caps_malloc(total, MALLOC_CAP_8BIT);
  if (!pkt) return false;

  const size_t written = proto::build_open_url_packet(url, flags, pkt, total);
  bool ok = false;
  if (written) {
    if (xSemaphoreTake(ws_send_mtx_, pdMS_TO_TICKS(50)) == pdTRUE) {
      const int r = esp_websocket_client_send_bin(ws_client_, (const char *) pkt, (int) written, pdMS_TO_TICKS(200));
      xSemaphoreGive(ws_send_mtx_);
      ok = (r == (int) written);
    }
  }
  free(pkt);
  return ok;
}

bool RemoteWebView::ws_send_keepalive_() {
  if (!ws_client_ || !ws_send_mtx_ || !esp_websocket_client_is_connected(ws_client_))
    return false;

  uint8_t pkt[sizeof(proto::KeepalivePacket)];
  const size_t n = proto::build_keepalive_packet(pkt);
  if (!n) return false;

  const TickType_t to = pdMS_TO_TICKS(50);
  if (xSemaphoreTake(ws_send_mtx_, to) != pdTRUE)
    return false;

  const int r = esp_websocket_client_send_bin(ws_client_, (const char*)pkt, (int)n, to);
  xSemaphoreGive(ws_send_mtx_);
  return r == (int)n;
}

void RemoteWebViewTouchListener::update(const touchscreen::TouchPoints_t &pts) {
  if (!parent_) return;

  const uint64_t now = esp_timer_get_time();
  for (auto &p : pts) {
    switch (p.state) {
      case touchscreen::STATE_PRESSED:
        parent_->ws_send_touch_event_(proto::TouchType::Down, p.x, p.y, p.id);
        break;
      case touchscreen::STATE_UPDATED:
        if (!RemoteWebView::kCoalesceMoves || RemoteWebView::kMoveIntervalUs == 0 ||
            (now - parent_->last_move_us_) >= RemoteWebView::kMoveIntervalUs) {
          parent_->last_move_us_ = now;
          parent_->ws_send_touch_event_(proto::TouchType::Move, p.x, p.y, p.id);
        }
        break;
      case touchscreen::STATE_RELEASING:
      case touchscreen::STATE_RELEASED:
        parent_->ws_send_touch_event_(proto::TouchType::Up, p.x, p.y, p.id);
        break;
      default: break;
    }
  }
}

void RemoteWebViewTouchListener::release() {
  if (!parent_) return;
  
  parent_->ws_send_touch_event_(proto::TouchType::Up, 0, 0, 0);
}

void RemoteWebViewTouchListener::touch(touchscreen::TouchPoint tp) {
  if (!parent_) return;
  
  parent_->ws_send_touch_event_(proto::TouchType::Down, tp.x, tp.y, tp.id);
}

void RemoteWebView::disable_touch(bool disable) {
  touch_disabled_ = disable;
  ESP_LOGD(TAG, "touch %s", disable ? "disabled" : "enabled");
}

void RemoteWebView::set_server(const std::string &s) {
  auto pos = s.rfind(':');
  if (pos == std::string::npos || pos == s.size() - 1) {
    ESP_LOGE(TAG, "server must be host:port, got: %s", s.c_str());
    return;
  }
  server_host_ = s.substr(0, pos);
  server_port_ = atoi(s.c_str() + pos + 1);
  if (server_port_ <= 0 || server_port_ > 65535) {
    ESP_LOGE(TAG, "invalid port in server: %s", s.c_str());
    server_host_.clear();
    server_port_ = 0;
  }
}

void RemoteWebView::append_q_int_(std::string &s, const char *k, int v) {
  if (v < 0) return;
  s += (s.find('?') == std::string::npos) ? '?' : '&';
  char buf[32];
  snprintf(buf, sizeof(buf), "%s=%d", k, v);
  s += buf;
}

void RemoteWebView::append_q_float_(std::string &s, const char *k, float v) {
  if (v < 0.0f) return;
  s += (s.find('?') == std::string::npos) ? '?' : '&';
  char buf[32];
  
  snprintf(buf, sizeof(buf), "%s=%.2f", k, (double)v);
  s += buf;
}

void RemoteWebView::append_q_str_(std::string &s, const char *k, const char *v) {
  if (!v || !*v) return;
  s += (s.find('?') == std::string::npos) ? '?' : '&';
  s += k; s += '='; s += v;
}

std::string RemoteWebView::resolve_device_id_() const {
  if (!device_id_.empty()) return device_id_;

  uint8_t mac[6] = {0};
  esp_err_t err = ESP_FAIL;
  
#if ESP_IDF_VERSION_MAJOR >= 5
  err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
  if (err != ESP_OK) {
    err = esp_read_mac(mac, ESP_MAC_BT);
  }
  if (err != ESP_OK) {
    err = esp_read_mac(mac, ESP_MAC_ETH);
  }
  if (err != ESP_OK) {
    err = esp_efuse_mac_get_default(mac);
  }
#else
  err = esp_efuse_mac_get_default(mac);
  if (err != ESP_OK) {
    err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
  }
#endif

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to read MAC address, using random ID");
    snprintf((char*)mac, sizeof(mac), "%06lx", (unsigned long)esp_random());
  }

  char buf[32];
  snprintf(buf, sizeof(buf), "esp32-%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return std::string(buf);
}

std::string RemoteWebView::build_ws_uri_() const {
  std::string uri;
  uri = "ws://" + server_host_ + ":" + std::to_string(server_port_);
  uri += "/";

  const std::string id = resolve_device_id_();
  append_q_str_(uri, "id", id.c_str());

  append_q_int_(uri, "w", display_width_);
  append_q_int_(uri, "h", display_height_);

  append_q_int_(uri,   "r",    rotation_);
  append_q_int_(uri,   "ts",   tile_size_);
  append_q_int_(uri,   "fftc", full_frame_tile_count_);
  append_q_float_(uri, "ffat", full_frame_area_threshold_);
  append_q_int_(uri,   "ffe",  full_frame_every_);
  append_q_int_(uri,   "enf",  every_nth_frame_);
  append_q_int_(uri,   "mfi",  min_frame_interval_);
  append_q_int_(uri,   "q",    jpeg_quality_);
  append_q_int_(uri,   "mbpm", max_bytes_per_msg_);

  return uri;
}

}  // namespace remote_webview
}  // namespace esphome
