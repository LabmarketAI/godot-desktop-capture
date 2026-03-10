#pragma once

#ifdef _WIN32

#include "backend_base.h"

#include <atomic>
#include <functional>
#include <string>
#include <thread>

/// Windows.Graphics.Capture backend for DesktopCaptureTexture.
///
/// Works from any process regardless of DWM composition state, so it continues
/// delivering frames even when an OpenXR/VR runtime has exclusive display
/// access (the scenario where IDXGIOutputDuplication only returns WAIT_TIMEOUT).
///
/// Requires Windows 10 version 1903 (build 18362) or later for
/// GraphicsCaptureSession and Direct3D11CaptureFramePool.
/// Direct3D11CaptureFramePool::CreateFreeThreaded (no dispatcher queue needed)
/// was added in Windows 10 2004 (build 19041).
class WGCCaptureBackend : public CaptureBackend {
public:
	WGCCaptureBackend() = default;
	~WGCCaptureBackend() override;

	bool start(int monitor_index, bool capture_cursor, int max_fps,
			std::function<void(const uint8_t *, int32_t, int32_t)> callback,
			std::string &error_out) override;

	void stop() override;

	void set_log_callback(std::function<void(const std::string &)> cb) override {
		_log_callback = std::move(cb);
	}

	void set_error_callback(std::function<void(const std::string &)> cb) override {
		_error_callback = std::move(cb);
	}

	// Static helpers — usable before start().
	static int enumerate_monitor_count();
	static bool get_monitor_size(int index, int32_t &out_w, int32_t &out_h);

private:
	void _log(const std::string &msg);

	std::atomic<bool> _running{ false };
	std::thread _thread;
	std::function<void(const uint8_t *, int32_t, int32_t)> _frame_callback;
	std::function<void(const std::string &)> _log_callback;
	std::function<void(const std::string &)> _error_callback;
};

#endif // _WIN32
