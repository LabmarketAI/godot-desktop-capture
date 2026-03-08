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

	// Emitted after each new frame is written to the texture.
	ADD_SIGNAL(MethodInfo("frame_updated"));

	// Emitted when the capture loop successfully initialises.
	ADD_SIGNAL(MethodInfo("capture_started"));

	// Emitted when the capture loop stops.
	// reason is a short snake_case string: "disabled", "permission_denied",
	// "device_lost", "missing_dependency", "monitor_index_out_of_range".
	ADD_SIGNAL(MethodInfo("capture_stopped",
			PropertyInfo(Variant::STRING, "reason")));
}

DesktopCaptureTexture::DesktopCaptureTexture() {
	// Create a 1×1 black placeholder so _get_rid() always returns a valid RID.
	// Backends replace the texture contents via _push_frame(); the RID itself
	// never changes, so materials assigned before capture starts keep working.
	Ref<Image> placeholder = Image::create_empty(1, 1, false, Image::FORMAT_RGB8);
	_texture_rid = RenderingServer::get_singleton()->texture_2d_create(placeholder);
}

DesktopCaptureTexture::~DesktopCaptureTexture() {
	// TODO (#4 / #5): stop the backend capture thread before freeing.
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
	_enabled = p_enabled;
	// TODO (#4 / #5): start or stop the platform backend here.
	// On failure the backend must call:
	//   _enabled = false;
	//   emit_signal("capture_stopped", reason_string);
}

bool DesktopCaptureTexture::get_enabled() const {
	return _enabled;
}

void DesktopCaptureTexture::set_monitor_index(int p_index) {
	_monitor_index = p_index;
	// TODO (#4 / #5): if currently enabled, restart the backend on the new monitor.
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

// --- Methods ---

int DesktopCaptureTexture::get_monitor_count() const {
	// TODO (#4 / #5): query the platform backend for the real count.
	return 1;
}

Vector2i DesktopCaptureTexture::get_monitor_size(int p_index) const {
	// TODO (#4 / #5): query the platform backend for the real size.
	return Vector2i(1920, 1080);
}

// --- Internal: called by platform backends on the render thread ---

void DesktopCaptureTexture::_push_frame(const Ref<Image> &p_image, int32_t p_width, int32_t p_height) {
	// This method is called from RenderingServer::call_on_render_thread() by
	// the platform backend.  It runs on the render thread — do not call from
	// the capture thread directly.
	ERR_FAIL_COND(!_texture_rid.is_valid());
	ERR_FAIL_COND(p_image.is_null());

	if (p_width != _width || p_height != _height) {
		// Resolution changed (monitor reconfigured, first real frame, etc.).
		// Recreate the RenderingServer texture at the new size.
		RenderingServer::get_singleton()->free_rid(_texture_rid);
		_texture_rid = RenderingServer::get_singleton()->texture_2d_create(p_image);
		_width = p_width;
		_height = p_height;
	} else {
		RenderingServer::get_singleton()->texture_2d_update(_texture_rid, p_image, 0);
	}

	emit_changed(); // invalidates materials that hold this texture
	emit_signal("frame_updated");
}
