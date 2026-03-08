#pragma once

#ifdef _WIN32

// Require Windows 8+ for IDXGIOutputDuplication (DXGI 1.2)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

// Callback invoked from the capture thread each time a new frame is ready.
//   data          — row-major RGBA8 pixels (B↔R already swapped from DXGI BGRA)
//   width, height — frame dimensions in pixels
// The callback must return quickly; heavy work should be offloaded.
using DXGIFrameCallback =
		std::function<void(const uint8_t *data, int32_t width, int32_t height)>;

/// Manages a DXGI Desktop Duplication capture loop on a background thread.
///
/// Usage:
///   DXGICaptureBackend backend;
///   std::string err;
///   if (!backend.start(0, true, 60, my_callback, err)) { ... }
///   // ... frames arrive via callback ...
///   backend.stop(); // joins thread, safe to call multiple times
class DXGICaptureBackend {
public:
	DXGICaptureBackend() = default;
	~DXGICaptureBackend();

	// Start capturing monitor at monitor_index.
	// Returns false and writes a snake_case reason into error_out on failure.
	bool start(int monitor_index, bool capture_cursor, int max_fps,
			DXGIFrameCallback callback, std::string &error_out);

	// Stop the capture loop and join the thread.  Idempotent.
	void stop();

	// Static helpers — no D3D device required, usable before start().
	static int enumerate_monitor_count();
	static bool get_monitor_size(int index, int32_t &out_w, int32_t &out_h);

private:
	// Initialise D3D11 device on the adapter that owns the target output.
	// Also creates IDXGIOutputDuplication.  Sets _frame_width/_height from
	// the output descriptor.
	bool _init(int monitor_index, std::string &error_out);

	// Release IDXGIOutputDuplication only (D3D device kept alive for reinit).
	void _release_duplication();

	// Release D3D device, context, and staging texture.
	void _release_d3d();

	// Ensure the staging texture matches the given dimensions.
	// Returns true if a new texture was allocated.
	bool _ensure_staging(int32_t w, int32_t h);

	// Capture loop executed on _thread.
	void _capture_loop();

	// Attempt to re-create the duplication handle after ACCESS_LOST.
	// Returns false if all retries exhausted.
	bool _reinit_duplication();

	// Fetch the current pointer shape from the duplication into _cursor_shape.
	bool _update_cursor_shape(IDXGIOutputDuplication *dup,
			const DXGI_OUTDUPL_FRAME_INFO &info);

	// Composite the cached cursor shape onto an RGBA8 frame buffer in-place.
	void _composite_cursor(uint8_t *rgba, int32_t frame_w, int32_t frame_h,
			DXGI_OUTDUPL_POINTER_POSITION cursor_pos);

	// ---- D3D / DXGI objects ----
	ID3D11Device *_device = nullptr;
	ID3D11DeviceContext *_context = nullptr;
	IDXGIOutputDuplication *_duplication = nullptr;
	ID3D11Texture2D *_staging = nullptr;
	int32_t _staging_w = 0;
	int32_t _staging_h = 0;

	// ---- Config (written before thread start, read-only in loop) ----
	int _monitor_index = 0;
	bool _capture_cursor_enabled = true;
	int _max_fps = 60;
	int32_t _frame_width = 0;
	int32_t _frame_height = 0;

	// ---- Thread ----
	std::atomic<bool> _running{ false };
	std::thread _thread;
	DXGIFrameCallback _frame_callback;

	// ---- Cursor shape cache ----
	struct CursorShape {
		DXGI_OUTDUPL_POINTER_SHAPE_TYPE type{};
		int32_t width = 0;
		int32_t height = 0;
		int32_t pitch = 0; // bytes per row in data
		int32_t hot_x = 0;
		int32_t hot_y = 0;
		std::vector<uint8_t> data;
	} _cursor_shape;
	bool _cursor_valid = false;
};

#endif // _WIN32
