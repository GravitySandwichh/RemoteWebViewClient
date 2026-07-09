#pragma once

namespace esphome {
namespace remote_webview {
namespace cfg {

// Bump with every release so `dump_config` output and idf_component.yml agree
// on what's actually flashed.
inline constexpr const char *component_version = "1.4.5";

// Per decode worker. Was 48KB ("headroom" inherited from the single-task
// era with no measurement behind it); with two workers that ate 96KB of
// internal SRAM and starved esp_websocket_client_init's ~60KB of rx/tx
// buffers at boot — the v1.4.1 boot loop. The decode call graph is shallow
// (iterative JPEGDEC -> draw callback -> esp_lcd memcpy, plus log
// formatting), so 32KB is still several times the realistic worst case.
inline constexpr int decode_task_stack = 32 * 1024;
// Worker 0 lives on core 1 (dedicated to decode, above everything there).
inline constexpr int decode_task_prio_core1 = 6;
// Worker 1 lives on core 0 with WiFi/lwIP/the WS client task (prio 5). EQUAL
// priority to the WS task, so FreeRTOS time-slices them per tick: both make
// steady progress. It was originally 4 (strictly below), which let the WS
// task starve the worker for hundreds of ms mid-message under load — it
// would dequeue a message, stall before the frame barrier while newer frames
// raced past on core 1, then drop it as stale, holding a reassembly slot
// hostage the whole time. A stalled worker is worse than no worker.
inline constexpr int decode_task_prio_core0 = 5;
// Under a sustained backlog, give the idle task (and thus the Task Watchdog)
// a scheduling slice after this many back-to-back decoded messages.
inline constexpr uint32_t decode_yield_every = 8;
// WS task stack bumped for stability under high throughput
inline constexpr int ws_task_stack = 12 * 1024;
inline constexpr int ws_task_prio = 5;
// Also sizes the reassembly buffer pool (kReasmPoolSize). With two workers
// each holding a slot mid-decode, 4 left only 2 slots to absorb bursts —
// a refinement pass (4 messages) landing while an animation resumed
// exhausted the pool and dropped tiles ("reassembly pool exhausted" logs).
// 8 slots x 32KB lives in plentiful PSRAM; stale-frame protection is
// handled by the frame barrier, not by keeping the queue starved.
inline constexpr int decode_queue_depth = 8;

// Application-level max: if a WS message payload exceeds this, it is dropped.
// The YAML max_bytes_per_msg (32768) overrides this at runtime per device.
inline constexpr size_t ws_max_message_bytes = 64 * 1024;
// TCP transport receive buffer — allocated from internal SRAM by the WS client
// library. KEEP THIS SMALL (<= 30 KB). It is independent of application message
// size: large messages arrive in chunks via payload_offset and are reassembled
// by WsReasm in PSRAM. 64 KB here caused OOM in IRAM -> client init failed.
inline constexpr size_t ws_buffer_size       = 30 * 1024;
inline constexpr size_t ws_keepalive_interval_us = 60 * 1000 * 1000;

inline constexpr bool coalesce_moves = true;
inline constexpr uint32_t move_rate_hz = 60;

} // namespace cfg
} // namespace remote_webview
} // namespace esphome
