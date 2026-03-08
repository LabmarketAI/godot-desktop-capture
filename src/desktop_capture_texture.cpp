#include "desktop_capture_texture.h"

#include <godot_cpp/core/class_db.hpp>

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

	ClassDB::bind_method(D_METHOD("set_capture_cursor", "capture"), &DesktopCaptureTexture::set_capture_cursor);
	ClassDB::bind_method(D_METHOD("get_capture_cursor"), &DesktopCaptureTexture::get_capture_cursor);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "capture_cursor"), "set_capture_cursor", "get_capture_cursor");

	ClassDB::bind_method(D_METHOD("set_max_fps", "fps"), &DesktopCaptureTexture::set_max_fps);
	ClassDB::bind_method(D_METHOD("get_max_fps"), &DesktopCaptureTexture::get_max_fps);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_fps", PROPERTY_HINT_RANGE, "1,240,1"),
			"set_max_fps", "get_max_fps");

	// --- Methods ---

	ClassDB::bind_method(D_METHOD("get_monitor_count"), &DesktopCaptureTexture::get_monitor_count);
	ClassDB::bind_method(D_METHOD("get_monitor_size", "index"), &DesktopCaptureTexture::get_monitor_size);

	// --- Signals ---

	/// Emitted after each new frame is written to the texture.
	ADD_SIGNAL(MethodInfo("frame_updated"));

	/// Emitted when the capture loop successfully initialises.
	ADD_SIGNAL(MethodInfo("capture_started"));

	/// Emitted when the capture loop stops.  reason is a short snake_case
	/// string: "disabled", "permission_denied", "device_lost",
	/// "missing_dependency", etc.
	ADD_SIGNAL(MethodInfo("capture_stopped",
			PropertyInfo(Variant::STRING, "reason")));
}

DesktopCaptureTexture::DesktopCaptureTexture() {}
DesktopCaptureTexture::~DesktopCaptureTexture() {}

// --- Texture2D virtual overrides ---

int32_t DesktopCaptureTexture::_get_width() const {
	// Stub: return a sensible default until a backend is wired in (#4, #5).
	return 1920;
}

int32_t DesktopCaptureTexture::_get_height() const {
	return 1080;
}

bool DesktopCaptureTexture::_has_alpha() const {
	return false;
}

// --- Property accessors ---

void DesktopCaptureTexture::set_enabled(bool p_enabled) {
	_enabled = p_enabled;
	// TODO (#4 / #5): start or stop the platform capture loop here.
}

bool DesktopCaptureTexture::get_enabled() const {
	return _enabled;
}

void DesktopCaptureTexture::set_monitor_index(int p_index) {
	_monitor_index = p_index;
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
	_max_fps = p_fps > 0 ? p_fps : 1;
}

int DesktopCaptureTexture::get_max_fps() const {
	return _max_fps;
}

// --- Methods ---

int DesktopCaptureTexture::get_monitor_count() const {
	// TODO (#4 / #5): query the platform backend for the real count.
	return 1;
}

Vector2i DesktopCaptureTexture::get_monitor_size(int p_index) const {
	// TODO (#4 / #5): query the platform backend for the real size.
	return Vector2i(1920, 1080);
}
