#pragma once

#if defined(_WIN32) || defined(__linux__)

#include <cstdint>
#include <functional>
#include <string>

/// Minimal abstract interface shared by all platform capture backends.
/// Allows DesktopCaptureTexture to swap WGCCaptureBackend / DXGICaptureBackend
/// at runtime without knowing the concrete type.
struct CaptureBackend {
	virtual ~CaptureBackend() = default;
	virtual bool start(int monitor_index, bool capture_cursor, int max_fps,
			std::function<void(const uint8_t *, int32_t, int32_t)> callback,
			std::string &error_out) = 0;
	virtual void stop() = 0;
	virtual void set_log_callback(std::function<void(const std::string &)> cb) = 0;
        virtual void set_error_callback(std::function<void(const std::string &)> cb) = 0;
};

#endif // _WIN32 || __linux__
