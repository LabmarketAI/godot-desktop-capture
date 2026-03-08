#pragma once

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#ifdef _WIN32
#include "backend_windows.h"
#endif

#ifdef __linux__
#include "backend_linux.h"
#endif

namespace godot {

/// A Texture2D that streams a live capture of an OS desktop monitor.
///
/// Assign to any StandardMaterial3D.albedo_texture or Viewport2Din3D texture
/// slot to display a live mirror of the host desktop inside a Godot scene.
///
/// ## Thread model
///
/// A background std::thread runs the platform capture loop.  When a new frame
/// arrives the capture thread calls
///   RenderingServer::get_singleton()->call_on_render_thread(upload_callable)
/// The upload callable runs on the render thread and calls
///   RenderingServer::get_singleton()->texture_2d_update(_texture_rid, image, 0)
/// followed by emit_changed() so materials using this texture redraw.
/// No Godot thread-safety locks are needed on the GDScript side.
///
/// ## Error handling
///
/// `enabled` is always truthful.  If set_enabled(true) is called but the
/// platform backend fails to initialise, _enabled is reverted to false and
/// capture_stopped(reason) is emitted.  Callers should listen to that signal
/// rather than polling enabled after assignment.
///
/// ## Platform backends
///   Windows — DXGI Desktop Duplication API          (issue #4)
///   Linux   — xdg-desktop-portal + PipeWire DMA-BUF (issue #5)
class DesktopCaptureTexture : public Texture2D {
	GDCLASS(DesktopCaptureTexture, Texture2D)

private:
	// RenderingServer texture RID owned by this object.
	// Created in the constructor with a 1×1 black placeholder so _get_rid()
	// always returns a valid RID even before a backend is active.
	RID _texture_rid;

	// Current frame dimensions — updated by the backend when capture starts
	// or the monitor resolution changes.
	// Initialised to 1×1 to match the placeholder texture created in the
	// constructor; _push_frame() recreates the RID on the first real frame.
	int32_t _width = 1;
	int32_t _height = 1;

	// User-facing properties.
	bool _enabled = false; // false until explicitly started; avoids auto-capture on load
	int _monitor_index = 0;
	bool _capture_cursor = true;
	int _max_fps = 60;

	// Platform backend (owned; null when no backend is active or on unsupported platform).
#ifdef _WIN32
	DXGICaptureBackend *_backend = nullptr;
#elif defined(__linux__)
	PipeWireCaptureBackend *_backend = nullptr;
#endif

protected:
	static void _bind_methods();

public:
	DesktopCaptureTexture();
	~DesktopCaptureTexture();

	// --- Texture2D virtual overrides ---

	/// Returns the RenderingServer RID for this texture.
	/// The RID is valid for the lifetime of this object; backends update it
	/// in-place via RenderingServer::texture_2d_update().
	virtual RID _get_rid() const override;

	/// Returns the pixel width of the current frame (1920 until a backend
	/// updates _width).
	virtual int32_t _get_width() const override;

	/// Returns the pixel height of the current frame (1080 until a backend
	/// updates _height).
	virtual int32_t _get_height() const override;

	/// Returns false — desktop frames do not carry an alpha channel.
	virtual bool _has_alpha() const override;

	// --- Properties ---

	/// Enable or disable the capture loop.
	/// Setting to true starts the platform backend; false stops it and emits
	/// capture_stopped("disabled").  If the backend fails to start, this
	/// reverts to false and capture_stopped(reason) is emitted.
	void set_enabled(bool p_enabled);
	bool get_enabled() const;

	/// Zero-based index of the monitor to capture.
	/// Use get_monitor_count() to check how many monitors are available.
	/// Changing this while enabled restarts the capture loop on the new monitor.
	void set_monitor_index(int p_index);
	int get_monitor_index() const;

	/// When true, the OS mouse cursor is composited onto each captured frame.
	void set_capture_cursor(bool p_capture);
	bool get_capture_cursor() const;

	/// Maximum capture rate in frames per second (1–240).
	/// Lower values reduce GPU load at the cost of display latency.
	void set_max_fps(int p_fps);
	int get_max_fps() const;

	// --- Methods ---

	/// Returns the number of monitors detected by the platform backend.
	/// Returns 1 in the stub implementation.
	int get_monitor_count() const;

	/// Returns the pixel dimensions of the monitor at p_index.
	/// Returns Vector2i(1920, 1080) in the stub implementation.
	Vector2i get_monitor_size(int p_index) const;

	// --- Internal helpers called by platform backends (not GDScript-visible) ---

	/// Called by a backend on the render thread to push a new frame.
	/// Updates the RenderingServer texture in-place and emits frame_updated.
	void _push_frame(const Ref<Image> &p_image, int32_t p_width, int32_t p_height);
};

} // namespace godot
