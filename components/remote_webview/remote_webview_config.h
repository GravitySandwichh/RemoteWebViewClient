#pragma once

namespace esphome {
namespace remote_webview {
namespace cfg {

// Bump with every release so `dump_config` output and idf_component.yml agree
// on what's actually flashed.
inline constexpr const char *component_version = "1.6.0";

// Per decode worker. Was 48KB ("headroom" inherited from the single-task
// era with no measurement behind it); with two workers that ate 96KB of
// internal SRAM and starved esp_websocket_client_init's ~60KB of rx/tx
// buffers at boot — the v1.4.1 boot loop. The decode call graph is shallow
// (iterative JPEGDEC -> draw callback -> esp_lcd memcpy, plus log
// formatting), so 32KB is still several times the realistic worst case.
inline constexpr int decode_task_stack = 32 * 1024;
// Worker 0 lives on core 1 (dedicated to decode, above everything there).
inline constexpr int decode_task_prio_core1 = 6;
// Worker 1 lives on core 0. Priority history matters here:
//   - 4 (below the WS task, 5): starved for hundreds of ms mid-message,
//     dropped frames as stale, held reassembly slots hostage.
//   - 5 (equal to WS): still starved — the real preemptors were never the
//     WS task but lwIP (18) and the WiFi driver (23), which own core 0
//     exactly when frames flood in.
//   - 19 (current): above lwIP, below the WiFi driver and esp_timer (22).
//     The worker can only be preempted for brief radio-critical work, so it
//     decodes its ~10ms strip uninterrupted; TCP ACK processing waits those
//     ~10ms, which TCP absorbs without blinking. This is what makes two
//     strips per core actually run in parallel — full-frame wall time is
//     4 serial strip decodes (~44ms) single-core vs ~2 (~24-27ms) dual.
//     The periodic decode_yield_every vTaskDelay below doubles as the valve
//     that lets lwIP and the main loop breathe during sustained backlogs.
inline constexpr int decode_task_prio_core0 = 19;
// Under a sustained backlog, give lower-priority tasks (idle for the Task
// Watchdog, and — for the priority-19 core-0 worker — lwIP and the main
// loop) a scheduling slice after this many back-to-back decoded messages.
// Tightened from 8 to 4 when worker 1 moved above lwIP: the valve has to
// open often enough that TCP ACK processing never waits tens of ms.
inline constexpr uint32_t decode_yield_every = 4;
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
