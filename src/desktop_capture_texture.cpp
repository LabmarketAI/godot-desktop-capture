#include "desktop_capture_texture.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/method_bind.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <cstring>

#ifdef _WIN32
#include "backend_wgc.h"
#include "backend_windows.h"
#endif
#ifdef __linux__
#include "backend_linux.h"
#endif

using namespace godot;

void DesktopCaptureTexture::_bind_methods() {
	// --- Properties ---

	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &DesktopCaptureTexture::set_enabled);
	ClassDB::bind_method(D_METHOD("get_enabled"), &DesktopCaptureTexture::get_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "get_enabled");

	ClassDB::bind_method(D_METHOD("set_monitor_index", "index"), &DesktopCaptureTexture::set_monitor_index);
	ClassDB::bind_method(D_METHOD("get_monitor_index"), &DesktopCaptureTexture::get_monitor_index);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "monitor_index", PROPERTY_HINT_RANGE, "0,8,1"),
			"set_monitor_index", "get_monitor_index");

     ClassDB::bind_method(D_METHOD("set_window_id", "id"), &DesktopCaptureTexture::set_window_id);
 ClassDB::bind_method(D_METHOD("get_window_id"), &DesktopCaptureTexture::get_window_id);
 ADD_PROPERTY(PropertyInfo(Variant::INT, "window_id"), "set_window_id", "get_window_id");

	ClassDB::bind_method(D_METHOD("set_capture_cursor", "capture"), &DesktopCaptureTexture::set_capture_cursor);
	ClassDB::bind_method(D_METHOD("get_capture_cursor"), &DesktopCaptureTexture::get_capture_cursor);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "capture_cursor"), "set_capture_cursor", "get_capture_cursor");

	ClassDB::bind_method(D_METHOD("set_max_fps", "fps"), &DesktopCaptureTexture::set_max_fps);
	ClassDB::bind_method(D_METHOD("get_max_fps"), &DesktopCaptureTexture::get_max_fps);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_fps", PROPERTY_HINT_RANGE, "1,240,1"),
			"set_max_fps", "get_max_fps");

	ClassDB::bind_method(D_METHOD("set_diagnostics_enabled", "enabled"), &DesktopCaptureTexture::set_diagnostics_enabled);
	ClassDB::bind_method(D_METHOD("get_diagnostics_enabled"), &DesktopCaptureTexture::get_diagnostics_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "diagnostics_enabled"), "set_diagnostics_enabled", "get_diagnostics_enabled");

	// --- Methods ---

       ClassDB::bind_method(D_METHOD("get_available_windows"), &DesktopCaptureTexture::get_available_windows);

	ClassDB::bind_method(D_METHOD("get_monitor_count"), &DesktopCaptureTexture::get_monitor_count);
	ClassDB::bind_method(D_METHOD("get_monitor_size", "index"), &DesktopCaptureTexture::get_monitor_size);
	ClassDB::bind_method(D_METHOD("get_capture_stats"), &DesktopCaptureTexture::get_capture_stats);
	ClassDB::bind_method(D_METHOD("reset_capture_stats"), &DesktopCaptureTexture::reset_capture_stats);

	// Internal: deferred backend start (registered so call_deferred can invoke it).
	ClassDB::bind_method(D_METHOD("_start_backend"), &DesktopCaptureTexture::_start_backend);

	// Internal: called via call_deferred from the capture thread to push a new
	// frame onto the main thread (avoids ObjectDB thread-safety issues with
	// callable_mp + call_on_render_thread from GDExtension objects).
	ClassDB::bind_method(D_METHOD("_push_frame_deferred", "image"),
			&DesktopCaptureTexture::_push_frame_deferred);

	// --- Signals ---

	// Emitted after each new frame is written to the texture.
	ADD_SIGNAL(MethodInfo("frame_updated"));
	ADD_SIGNAL(MethodInfo("capture_stats_updated", PropertyInfo(Variant::DICTIONARY, "stats")));

	// Emitted when the capture loop successfully initialises.
	ADD_SIGNAL(MethodInfo("capture_started"));

	// Emitted when the capture loop stops.
	// reason is a short snake_case string: "disabled", "permission_denied",
	// "device_lost", "missing_dependency", "monitor_index_out_of_range".
	ADD_SIGNAL(MethodInfo("capture_stopped",
			PropertyInfo(Variant::STRING, "reason")));

	// Emitted when an error occurs during capture that might require a restart.
	ADD_SIGNAL(MethodInfo("capture_error",
			PropertyInfo(Variant::STRING, "error_message")));
}

DesktopCaptureTexture::DesktopCaptureTexture() {
	// Create a 1×1 black placeholder so _get_rid() always returns a valid RID.
	// Backends replace the texture contents via _push_frame(); the RID itself
	// never changes, so materials assigned before capture starts keep working.
	Ref<Image> placeholder = Image::create_empty(1, 1, false, Image::FORMAT_RGBA8);
	_texture_rid = RenderingServer::get_singleton()->texture_2d_create(placeholder);
	reset_capture_stats();
}

DesktopCaptureTexture::~DesktopCaptureTexture() {
#if defined(_WIN32) || defined(__linux__)
	if (_backend) {
		_backend->stop();
		delete _backend;
		_backend = nullptr;
	}
#endif
	if (_texture_rid.is_valid()) {
		RenderingServer::get_singleton()->free_rid(_texture_rid);
	}
}

// --- Texture2D virtual overrides ---

RID DesktopCaptureTexture::_get_rid() const {
	return _texture_rid;
}

int32_t DesktopCaptureTexture::_get_width() const {
	return _width;
}

int32_t DesktopCaptureTexture::_get_height() const {
	return _height;
}

bool DesktopCaptureTexture::_has_alpha() const {
	return false;
}

// --- Property accessors ---

void DesktopCaptureTexture::set_enabled(bool p_enabled) {
	if (_enabled == p_enabled) {
		return;
	}

	if (!p_enabled) {
		// Stop the active backend.
#if defined(_WIN32) || defined(__linux__)
		if (_backend) {
			_backend->stop();
			delete _backend;
			_backend = nullptr;
		}
#endif
		_enabled = false;
		emit_signal("capture_stopped", String("disabled"));
		return;
	}

	// Mark as enabled and defer the actual backend start to the next main-thread
	// frame.  This avoids a race during TSCN scene loading where the resource's
	// reference count can temporarily drop to zero between property-set time and
	// the material taking ownership, causing the destructor (and stop()) to fire
	// before any frames are captured.
	_enabled = true;
	reset_capture_stats();
	call_deferred("_start_backend");
}

void DesktopCaptureTexture::_start_backend() {
	// Called via call_deferred from set_enabled(true).  By this point the scene
	// loader has finished and all references to this resource are established.
	if (!_enabled) {
		return; // Disabled again before we ran — nothing to do.
	}
#if defined(_WIN32) || defined(__linux__)
	if (_backend) {
		return; // Already started (e.g. called twice).
	}
#endif

#ifdef _WIN32
	{
		// The frame callback runs on the backend's capture thread.  It wraps
		// the raw RGBA8 buffer in an Image and dispatches _push_frame_deferred()
		// to the main thread via call_deferred.  This avoids the ObjectDB
		// thread-safety issue that causes callable_mp + call_on_render_thread
		// to silently drop calls for GDExtension RefCounted objects.
		auto callback = [this](const uint8_t *data, int32_t w, int32_t h) {
			PackedByteArray bytes;
			bytes.resize(w * h * 4);
			memcpy(bytes.ptrw(), data, static_cast<size_t>(w * h * 4));
			Ref<Image> image = Image::create_from_data(w, h, false, Image::FORMAT_RGBA8, bytes);
			call_deferred("_push_frame_deferred", image);
		};

		auto log_cb = [](const std::string &msg) {
			WARN_PRINT(msg.c_str());
		};

		auto error_cb = [this](const std::string &msg) {
			call_deferred("emit_signal", "capture_error", String(msg.c_str()));
		};

		std::string error;

		// Try Windows.Graphics.Capture first — it works even when the VR runtime
		// suspends DWM desktop composition (where DXGI Desktop Duplication only
		// returns WAIT_TIMEOUT indefinitely).
		{
			WGCCaptureBackend *wgc = new WGCCaptureBackend();
			wgc->set_log_callback(log_cb);
			wgc->set_error_callback(error_cb);
			if (wgc->start(_monitor_index, _window_id, _capture_cursor, _max_fps, callback, error)) {
				_backend = wgc;
				emit_signal("capture_started");
				return;
			}
			// WGC unavailable (old Windows build, permission denied, etc.) —
			// fall through to DXGI.
			WARN_PRINT(("DesktopCapture: WGC backend failed (" + error +
					"), falling back to DXGI Desktop Duplication")
							.c_str());
			delete wgc;
		}

		// DXGI Desktop Duplication fallback.
		{
			DXGICaptureBackend *dxgi = new DXGICaptureBackend();
			dxgi->set_log_callback(log_cb);
			dxgi->set_error_callback(error_cb);
			if (!dxgi->start(_monitor_index, _window_id, _capture_cursor, _max_fps, callback, error)) {
				delete dxgi;
				_enabled = false;
				emit_signal("capture_stopped", String(error.c_str()));
				return;
			}
			_backend = dxgi;
			emit_signal("capture_started");
		}
	}
#elif defined(__linux__)
	{
		PipeWireCaptureBackend *backend = new PipeWireCaptureBackend();
		std::string error;

		auto callback = [this](const uint8_t *data, int32_t w, int32_t h) {
			PackedByteArray bytes;
			bytes.resize(w * h * 4);
			memcpy(bytes.ptrw(), data, static_cast<size_t>(w * h * 4));
			Ref<Image> image = Image::create_from_data(w, h, false, Image::FORMAT_RGBA8, bytes);
			call_deferred("_push_frame_deferred", image);
		};

		auto error_cb = [this](const std::string &msg) {
			call_deferred("emit_signal", "capture_error", String(msg.c_str()));
		};

		backend->set_error_callback(error_cb);

		if (!backend->start(_monitor_index, _window_id, _capture_cursor, _max_fps, callback, error)) {
			delete backend;
			_enabled = false;
			emit_signal("capture_stopped", String(error.c_str()));
			return;
		}
		_backend = backend;
		emit_signal("capture_started");
	}
#else
	// No backend available on this platform.
	_enabled = false;
	emit_signal("capture_stopped", String("unsupported_platform"));
#endif
}

bool DesktopCaptureTexture::get_enabled() const {
	return _enabled;
}

void DesktopCaptureTexture::set_monitor_index(int p_index) {
	if (p_index == _monitor_index) {
		return;
	}
	_monitor_index = p_index;
	if (_enabled) {
		// Restart the backend on the new monitor.
		set_enabled(false);
		set_enabled(true);
	}
}

void DesktopCaptureTexture::set_window_id(int64_t p_id) {
	if (p_id == _window_id) {
		return;
	}
	_window_id = p_id;
	if (_enabled) {
		// Restart the backend on the new window.
		set_enabled(false);
		set_enabled(true);
	}
}

int64_t DesktopCaptureTexture::get_window_id() const {
	return _window_id;
}

Array DesktopCaptureTexture::get_available_windows() const {
	Array result;
#ifdef _WIN32
	WGCCaptureBackend::enumerate_windows([&result](int64_t hwnd, const std::string& title) {
		Dictionary dict;
		dict["id"] = hwnd;
		dict["title"] = String::utf8(title.c_str());
		result.push_back(dict);
	});
#endif
	return result;
}

int DesktopCaptureTexture::get_monitor_index() const {
	return _monitor_index;
}

void DesktopCaptureTexture::set_capture_cursor(bool p_capture) {
	_capture_cursor = p_capture;
}

bool DesktopCaptureTexture::get_capture_cursor() const {
	return _capture_cursor;
}

void DesktopCaptureTexture::set_max_fps(int p_fps) {
	_max_fps = MAX(p_fps, 1);
}

int DesktopCaptureTexture::get_max_fps() const {
	return _max_fps;
}

void DesktopCaptureTexture::set_diagnostics_enabled(bool p_enabled) {
	_diagnostics_enabled = p_enabled;
}

bool DesktopCaptureTexture::get_diagnostics_enabled() const {
	return _diagnostics_enabled;
}

Dictionary DesktopCaptureTexture::get_capture_stats() const {
	Dictionary stats;
	stats["total_frames"] = static_cast<int64_t>(_stats_total_frames);
	stats["interval_samples"] = static_cast<int64_t>(_stats_interval_samples);
	stats["late_frame_count"] = static_cast<int64_t>(_stats_late_frame_count);
	stats["last_interval_ms"] = _stats_last_interval_ms;
	stats["avg_interval_ms"] = _stats_avg_interval_ms;
	stats["max_interval_ms"] = _stats_max_interval_ms;
	stats["estimated_capture_fps"] = _stats_avg_interval_ms > 0.0 ? (1000.0 / _stats_avg_interval_ms) : 0.0;
	stats["target_max_fps"] = _max_fps;
	stats["diagnostics_enabled"] = _diagnostics_enabled;
	return stats;
}

void DesktopCaptureTexture::reset_capture_stats() {
	_stats_total_frames = 0;
	_stats_interval_samples = 0;
	_stats_late_frame_count = 0;
	_stats_last_frame_time_s = 0.0;
	_stats_last_emit_time_s = 0.0;
	_stats_last_interval_ms = 0.0;
	_stats_avg_interval_ms = 0.0;
	_stats_max_interval_ms = 0.0;
}

// --- Methods ---

int DesktopCaptureTexture::get_monitor_count() const {
#ifdef _WIN32
	return DXGICaptureBackend::enumerate_monitor_count();
#elif defined(__linux__)
	return PipeWireCaptureBackend::enumerate_monitor_count();
#else
	return 0;
#endif
}

Vector2i DesktopCaptureTexture::get_monitor_size(int p_index) const {
#ifdef _WIN32
	int32_t w = 0, h = 0;
	if (DXGICaptureBackend::get_monitor_size(p_index, w, h)) {
		return Vector2i(w, h);
	}
#elif defined(__linux__)
	int32_t w = 0, h = 0;
	PipeWireCaptureBackend::get_monitor_size(p_index, w, h);
	return Vector2i(w, h);
#endif
	return Vector2i(0, 0);
}

// --- Internal: called via call_deferred from the capture thread ---

void DesktopCaptureTexture::_push_frame_deferred(const Ref<Image> &p_image) {
	// Runs on the main thread (queued via call_deferred from the capture thread).
	// Using call_deferred instead of call_on_render_thread + callable_mp avoids
	// the ObjectDB thread-safety issue where GDExtension RefCounted objects are
	// not found when looked up from the render thread, causing silent drops.
	ERR_FAIL_COND(!_texture_rid.is_valid());
	ERR_FAIL_COND(p_image.is_null());

	const int32_t p_width = p_image->get_width();
	const int32_t p_height = p_image->get_height();

	if (p_width != _width || p_height != _height) {
		// Resolution changed (monitor reconfigured, first real frame, etc.).
		// Recreate the RenderingServer texture at the new size.
		WARN_PRINT(("DesktopCapture: first frame " + std::to_string(p_width) + "x" + std::to_string(p_height) + " -- updating dimensions with texture_replace").c_str());
		RID new_rid = RenderingServer::get_singleton()->texture_2d_create(p_image);
		RenderingServer::get_singleton()->texture_replace(_texture_rid, new_rid);
		_width = p_width;
		_height = p_height;
	} else {
		RenderingServer::get_singleton()->texture_2d_update(_texture_rid, p_image, 0);
	}

	const double now_s = static_cast<double>(Time::get_singleton()->get_ticks_usec()) / 1000000.0;
	_stats_total_frames += 1;
	if (_stats_last_frame_time_s > 0.0) {
		const double interval_ms = (now_s - _stats_last_frame_time_s) * 1000.0;
		_stats_last_interval_ms = interval_ms;
		_stats_interval_samples += 1;
		_stats_avg_interval_ms += (interval_ms - _stats_avg_interval_ms) / static_cast<double>(_stats_interval_samples);
		if (interval_ms > _stats_max_interval_ms) {
			_stats_max_interval_ms = interval_ms;
		}

		const double target_interval_ms = 1000.0 / static_cast<double>(MAX(_max_fps, 1));
		if (interval_ms > target_interval_ms * 1.5) {
			_stats_late_frame_count += 1;
		}
	}
	_stats_last_frame_time_s = now_s;

	if (_diagnostics_enabled && (_stats_last_emit_time_s <= 0.0 || (now_s - _stats_last_emit_time_s) >= 1.0)) {
		_stats_last_emit_time_s = now_s;
		emit_signal("capture_stats_updated", get_capture_stats());
	}

	emit_changed(); // invalidates materials that hold this texture
	emit_signal("frame_updated");
}

// --- Legacy: kept for binary compatibility but no longer called internally ---

void DesktopCaptureTexture::_push_frame(const Ref<Image> &p_image, int32_t p_width, int32_t p_height) {
	_push_frame_deferred(p_image);
}
