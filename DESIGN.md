# Design Notes — godot-desktop-capture

This document records the architectural decisions for the `DesktopCaptureTexture`
GDExtension class.  It is the deliverable for
[issue #3](https://github.com/LabmarketAI/godot-desktop-capture/issues/3) and
should be updated whenever a decision is revisited.

---

## Decision 1 — Class hierarchy: extend `Texture2D` directly

**Chosen approach**: `DesktopCaptureTexture` extends `Texture2D` and owns a
`RenderingServer` texture `RID` created in its constructor.

**Alternatives considered**:

| Option | Notes | Rejected because |
|--------|-------|-----------------|
| Wrap an `ImageTexture` internally | Simpler RID management | Adds one layer of indirection; `ImageTexture` doesn't expose `texture_2d_update` semantics cleanly from GDExtension |
| Extend `ImageTexture` | Reuses RID management | `ImageTexture` is designed for static images; its internal update path goes through `Image` on the CPU, which we want to avoid on the fast path |
| **Extend `Texture2D` directly** ✅ | Owns the RID | Minimal, correct; `_get_rid()` returns the RID directly to the renderer; backends update in-place via `RS::texture_2d_update()` |

**Implementation**:

```cpp
// Constructor: 1×1 black placeholder — _get_rid() is valid immediately.
Ref<Image> placeholder = Image::create_empty(1, 1, false, Image::FORMAT_RGB8);
_texture_rid = RenderingServer::get_singleton()->texture_2d_create(placeholder);

// _get_rid() override:
RID DesktopCaptureTexture::_get_rid() const { return _texture_rid; }
```

The RID is freed in the destructor.  The same RID is reused across frames
(via `texture_2d_update`); it is only recreated when the frame resolution
changes (monitor reconfigure, first real frame arriving, etc.).

**Consequence**: assigning `DesktopCaptureTexture` to a material before calling
`enabled = true` is safe — a 1×1 black texture renders rather than a crash or
an invalid RID error.

---

## Decision 2 — Thread model

**Chosen approach**: capture thread → `RS::call_on_render_thread()` → `_push_frame()`

```
┌─────────────────────┐      new frame ready      ┌──────────────────────┐
│  Platform capture   │ ────────────────────────▶  │  RenderingServer     │
│  thread (std::thread│  RS::call_on_render_thread  │  render thread       │
│  Windows: DXGI loop │                             │                      │
│  Linux:  PW stream  │                             │  _push_frame():      │
└─────────────────────┘                             │  texture_2d_update() │
                                                    │  emit_changed()      │
                                                    │  emit("frame_updated")│
                                                    └──────────────────────┘
```

**Why not poll in a Node's `_process()`?**
`Texture2D` is a `Resource`, not a `Node` — it has no `_process()`.  Requiring
a companion `Node` would break the "drop onto any material" usage contract.
The capture thread pushing directly to the render thread is cleaner and has
lower latency.

**Thread safety contract**:
- The capture thread must **not** call GDScript-visible methods on
  `DesktopCaptureTexture` directly.
- All Godot API calls that modify rendering state (texture update, emit_changed,
  emit_signal) happen inside `_push_frame()`, which only runs on the render thread.
- `_enabled`, `_monitor_index`, `_max_fps`, `_capture_cursor` are written by
  the main thread and read by the capture thread.  Backends must treat these as
  hints sampled at frame boundaries — not hot-path atomics.

**Frame rate limiting**:
The capture thread sleeps between frames to honour `max_fps`.  The calculation
is: `sleep_us = 1_000_000 / max_fps - elapsed_capture_us`.

---

## Decision 3 — Error handling: `enabled` is always truthful

**Contract**:

| Event | Behaviour |
|-------|-----------|
| `enabled = true`, backend init succeeds | `_enabled` stays `true`, `capture_started` emitted |
| `enabled = true`, backend init fails | `_enabled` reverted to `false`, `capture_stopped(reason)` emitted |
| Capture running, device lost (DXGI `ACCESS_LOST`, PipeWire disconnect) | Backend attempts one reconnect; if that fails, `_enabled = false`, `capture_stopped("device_lost")` emitted |
| `enabled = false` by caller | Capture thread stopped, `capture_stopped("disabled")` emitted |
| Runtime lib missing (`libpipewire`, etc.) | `_enabled = false`, `capture_stopped("missing_dependency")` emitted |
| `monitor_index` out of range | `_enabled = false`, `capture_stopped("monitor_index_out_of_range")` emitted |

**`reason` string vocabulary** (snake_case, stable across versions):

| Value | Meaning |
|-------|---------|
| `"disabled"` | Caller set `enabled = false` |
| `"device_lost"` | GPU/display device was lost and could not be recovered |
| `"permission_denied"` | OS portal or security policy refused screen capture |
| `"missing_dependency"` | Required runtime library not found (`libpipewire`, etc.) |
| `"monitor_index_out_of_range"` | `monitor_index >= get_monitor_count()` |
| `"unsupported_platform"` | Running on a platform with no backend (macOS, etc.) |

**GDScript usage pattern**:

```gdscript
var capture := DesktopCaptureTexture.new()
capture.capture_stopped.connect(_on_capture_stopped)
capture.enabled = true

func _on_capture_stopped(reason: String) -> void:
    match reason:
        "permission_denied":
            push_warning("Screen capture permission denied.")
        "missing_dependency":
            push_error("PipeWire not available on this system.")
        _:
            push_warning("Capture stopped: " + reason)
```

---

## Public API summary

### Properties

| Name | Type | Default | Description |
|------|------|---------|-------------|
| `enabled` | `bool` | `false` | Start/stop the capture loop |
| `monitor_index` | `int` | `0` | Zero-based monitor index |
| `capture_cursor` | `bool` | `true` | Composite OS cursor onto frames |
| `max_fps` | `int` | `60` | Capture rate cap (1–240) |

> `enabled` defaults to **`false`** to prevent unexpected capture on scene
> load.  Set it to `true` explicitly in `_ready()` or via the Inspector.

### Signals

| Name | Args | Description |
|------|------|-------------|
| `frame_updated` | — | New frame written to the texture |
| `capture_started` | — | Backend initialised successfully |
| `capture_stopped` | `reason: String` | Capture stopped; see reason vocabulary above |

### Methods

| Signature | Description |
|-----------|-------------|
| `get_monitor_count() -> int` | Number of available monitors |
| `get_monitor_size(index: int) -> Vector2i` | Pixel dimensions of a monitor |

### Internal (C++ only, not GDScript-visible)

| Signature | Called from |
|-----------|------------|
| `_push_frame(image, width, height)` | Platform backend, on render thread via `RS::call_on_render_thread()` |
