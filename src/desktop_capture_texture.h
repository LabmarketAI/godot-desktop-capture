#pragma once

#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/vector2i.hpp>

namespace godot {

/// A Texture2D that streams a live capture of an OS desktop monitor.
///
/// Assign to any StandardMaterial3D.albedo_texture or Viewport2Din3D texture
/// slot to display a live mirror of the host desktop inside a Godot scene.
///
/// Platform backends:
///   Windows — DXGI Desktop Duplication API (implemented in #4)
///   Linux   — xdg-desktop-portal ScreenCast + PipeWire DMA-BUF (implemented in #5)
///
/// This file contains the stub implementation: the class loads and exposes
/// the full public API, but the capture loop is not yet wired to a backend.
/// Frame dimensions are reported as 1920×1080 until a backend is active.
class DesktopCaptureTexture : public Texture2D {
	GDCLASS(DesktopCaptureTexture, Texture2D)

private:
	bool _enabled = true;
	int _monitor_index = 0;
	bool _capture_cursor = true;
	int _max_fps = 60;

protected:
	static void _bind_methods();

public:
	DesktopCaptureTexture();
	~DesktopCaptureTexture();

	// --- Texture2D virtual overrides ---

	/// Returns the pixel width of the captured frame (or 1920 when no backend
	/// is active).
	virtual int32_t _get_width() const override;

	/// Returns the pixel height of the captured frame (or 1080 when no backend
	/// is active).
	virtual int32_t _get_height() const override;

	/// Returns false — desktop frames do not carry an alpha channel.
	virtual bool _has_alpha() const override;

	// --- Properties ---

	/// Start or stop the capture loop.  Setting to false stops the background
	/// thread and emits capture_stopped("disabled").
	void set_enabled(bool p_enabled);
	bool get_enabled() const;

	/// Zero-based index of the monitor to capture.  Use get_monitor_count() to
	/// determine how many monitors are available.
	void set_monitor_index(int p_index);
	int get_monitor_index() const;

	/// When true, the OS cursor is composited onto each captured frame.
	void set_capture_cursor(bool p_capture);
	bool get_capture_cursor() const;

	/// Maximum capture rate in frames per second.  Lower values reduce GPU load.
	void set_max_fps(int p_fps);
	int get_max_fps() const;

	// --- Methods ---

	/// Returns the number of monitors available on the host system.
	/// Returns 1 in the stub implementation.
	int get_monitor_count() const;

	/// Returns the pixel dimensions of the monitor at the given index.
	/// Returns Vector2i(1920, 1080) in the stub implementation.
	Vector2i get_monitor_size(int p_index) const;
};

} // namespace godot
