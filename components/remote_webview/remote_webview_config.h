#pragma once

namespace esphome {
namespace remote_webview {
namespace cfg {

// Bump with every release so `dump_config` output and idf_component.yml agree
// on what's actually flashed.
inline constexpr const char *component_version = "1.4.0";

// Decode task needs headroom for JPEGDEC + draw_pixels_at at high frame rates
inline constexpr int decode_task_stack = 48 * 1024;
// Worker 0 lives on core 1 (dedicated to decode, above everything there).
inline constexpr int decode_task_prio_core1 = 6;
// Worker 1 lives on core 0 with WiFi/lwIP/the WS client task (prio 5). Keep
// it BELOW the WS task so receiving the next message always preempts
// decoding the current one — it soaks up spare core-0 cycles only.
inline constexpr int decode_task_prio_core0 = 4;
// Under a sustained backlog, give the idle task (and thus the Task Watchdog)
// a scheduling slice after this many back-to-back decoded messages.
inline constexpr uint32_t decode_yield_every = 8;
// WS task stack bumped for stability under high throughput
inline constexpr int ws_task_stack = 12 * 1024;
inline constexpr int ws_task_prio = 5;
// Small queue intentional at 60fps: stale frames are worthless, drop them fast
inline constexpr int decode_queue_depth = 4;

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
