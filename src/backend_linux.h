#pragma once

#ifdef __linux__

#include "backend_base.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

// Forward declarations — full types defined in pipewire/dbus headers included
// by backend_linux.cpp only.  Header consumers see only the class interface.
struct pw_main_loop;
struct pw_context;
struct pw_core;
struct pw_stream;
struct pw_stream_events;
struct spa_hook;

// Callback invoked from the PipeWire data thread each time a new frame is ready.
//   data          — row-major RGBA8 pixels (B↔R already swapped from PipeWire BGRA)
//   width, height — frame dimensions in pixels
// The callback must return quickly; heavy work should be offloaded.
using PWFrameCallback =
		std::function<void(const uint8_t *data, int32_t width, int32_t height)>;

/// Captures a desktop monitor via the xdg-desktop-portal ScreenCast API
/// backed by a PipeWire stream.
///
/// Usage:
///   PipeWireCaptureBackend backend;
///   std::string err;
///   if (!backend.start(0, true, 60, my_callback, err)) { ... }
///   // ... frames arrive via callback on the PipeWire data thread ...
///   backend.stop(); // quits the PW main loop and joins the thread
///
/// Notes:
/// - libdbus-1.so.3 and libpipewire-0.3.so.0 are loaded via dlopen at runtime.
///   If either is absent, start() returns false with error "missing_dependency".
/// - The portal shows a compositor picker dialog on first use (Step 3/Start).
///   monitor_index is forwarded as a hint but the user ultimately decides.
/// - DMA-BUF buffers are skipped (SHM path only). Vulkan zero-copy is #6 / future.
class PipeWireCaptureBackend : public CaptureBackend {
public:
	PipeWireCaptureBackend() = default;
	~PipeWireCaptureBackend() override;

	// Start capturing. Runs the portal D-Bus flow synchronously, then launches
	// the PipeWire main loop on a background thread.
	// Returns false and writes a snake_case reason into error_out on failure.
	bool start(int monitor_index, int64_t window_id, bool capture_cursor, int max_fps,
			std::function<void(const uint8_t *, int32_t, int32_t)> callback,
			std::string &error_out) override;

	// Stop the capture loop and join the thread.  Idempotent.
	void stop() override;

	void set_log_callback(std::function<void(const std::string &)>) override {
		// Linux backend does not currently surface structured log callbacks.
	}

	void set_error_callback(std::function<void(const std::string &)> cb) override {
		_error_callback = std::move(cb);
	}

	// Static helpers — usable before start().
	// enumerate_monitor_count uses sysfs DRM to count connected outputs.
	static int enumerate_monitor_count();
	// get_monitor_size returns false on Linux — portal does not expose monitor
	// dimensions without an active session.
	static bool get_monitor_size(int index, int32_t &out_w, int32_t &out_h);

private:
	// Run the xdg-desktop-portal ScreenCast flow and retrieve the PipeWire
	// node ID. Fills _dbus_conn (kept alive for session lifetime).
	bool _portal_setup(int monitor_index, bool capture_cursor,
			uint32_t &out_node_id, std::string &error_out);

	// Create PipeWire objects and connect the stream to node_id.
	bool _pw_setup(uint32_t node_id, std::string &error_out);

	// Thread entry: runs pw_main_loop_run, then tears down PW objects.
	void _pw_thread_func();

	// PipeWire stream event callbacks (static — dispatched via userdata=this).
	static void _on_param_changed(void *data, uint32_t id,
			const struct spa_pod *param);
	static void _on_process(void *data);

	// ---- D-Bus (portal session lifetime) ----
	// Stored as void* to avoid pulling dbus headers into consumers of this header.
	// Cast to DBusConnection* inside backend_linux.cpp.
	void *_dbus_conn = nullptr;

	// ---- PipeWire objects ----
	struct pw_main_loop *_pw_loop = nullptr;
	struct pw_context *_pw_context = nullptr;
	struct pw_core *_pw_core = nullptr;
	struct pw_stream *_pw_stream = nullptr;
	// pw_stream_events and spa_hook must outlive the stream.
	// Allocated on heap to avoid pulling PW headers into this header.
	struct pw_stream_events *_pw_events = nullptr;
	struct spa_hook *_pw_listener = nullptr;

	// ---- Config ----
	int _monitor_index = 0;
	bool _capture_cursor_enabled = true;
	int _max_fps = 60;

	// ---- Frame state ----
	std::atomic<int32_t> _frame_width{ 0 };
	std::atomic<int32_t> _frame_height{ 0 };
	std::vector<uint8_t> _rgba_buf; // reused each frame on the PW data thread
	std::chrono::steady_clock::time_point _last_frame_time{};

	// ---- Thread ----
	std::atomic<bool> _running{ false };
	std::thread _pw_thread;
	PWFrameCallback _frame_callback;
	std::function<void(const std::string &)> _error_callback;
};

#endif // __linux__
