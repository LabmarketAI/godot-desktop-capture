# godot-desktop-capture

A Godot 4 GDExtension that streams a live capture of an OS desktop monitor into a `Texture2D`. Assign it to any material or `Viewport2Din3D` to display your desktop inside a Godot scene — useful for VR virtual-desktop setups, streaming overlays, and similar applications.

**Platform support**

| Platform | Backend | Status |
|----------|---------|--------|
| Windows 8+ | DXGI Desktop Duplication | ✅ Available |
| Linux (Wayland/X11) | xdg-desktop-portal + PipeWire | ✅ Available |
| macOS | — | Not planned |

---

## Installation

### Option A — Pre-built binary (recommended)

1. Download the latest `godot-desktop-capture-<version>.zip` from the [Releases](../../releases) page.
2. Extract the `addons/` folder into the root of your Godot project so the layout is:
   ```
   res://addons/godot-desktop-capture/
       godot-desktop-capture.gdextension
       bin/
           libgodot-desktop-capture.windows.release.x86_64.dll
           libgodot-desktop-capture.windows.debug.x86_64.dll
           libgodot-desktop-capture.linux.release.x86_64.so
           libgodot-desktop-capture.linux.debug.x86_64.so
   ```
3. Reload the project (`Project → Reload Current Project`). The `DesktopCaptureTexture` class will be available immediately — no plugin toggle required.

### Option B — Build from source

**Prerequisites**: Python 3, SCons, Git, MinGW-w64 or MSVC (Windows), GCC/Clang + `libpipewire-0.3-dev` + `libdbus-1-dev` (Linux).

SCons is a Python package. Install it system-wide or into a virtual environment:

```bash
# Option 1 — system-wide (simplest)
pip install scons

# Option 2 — virtual environment (keeps your global Python clean)
python -m venv .venv
source .venv/bin/activate   # Windows: .venv\Scripts\activate
pip install scons
# SCons is now on PATH for the duration of the activated shell.
```

```bash
git clone --recurse-submodules https://github.com/LabmarketAI/godot-desktop-capture.git
cd godot-desktop-capture

# Windows release build
scons target=template_release platform=windows arch=x86_64

# Windows debug build (used by the Godot editor)
scons target=template_debug platform=windows arch=x86_64

# Linux release build
scons target=template_release platform=linux arch=x86_64

# Linux debug build
scons target=template_debug platform=linux arch=x86_64
```

The compiled binary is written to `project/addons/godot-desktop-capture/bin/`. Copy the `project/addons/` folder into your project as described in Option A.

> **Windows note**: DXGI Desktop Duplication is a Windows 8+ API. No additional SDKs required — `dxgi.lib` and `d3d11.lib` ship with MinGW-w64 and MSVC.
>
> **Linux note**: `libpipewire` and `libdbus` are `dlopen`-ed at runtime — they are not linked at build time and are not bundled in the release zip. Any modern Wayland or X11 desktop with PipeWire (Ubuntu 22.04+, Fedora 34+, etc.) already has them.

### Local development with the godot-charts demo

#### Windows — automated script

`build_local.ps1` checks for every dependency, prompts to install anything missing, sets up the MSVC environment automatically (no Developer Command Prompt needed), builds the debug DLL, and copies it into the sibling `godot-charts` demo:

```powershell
cd godot-desktop-capture
.\build_local.ps1
```

Dependencies checked: Git, Python 3, SCons, MSVC (VS Build Tools). Missing items are offered for install via `winget`.

#### Manual steps

```bash
# From the godot-desktop-capture repo root (x64 Native Tools Command Prompt or equivalent):

# 1. Build the debug DLL (the Godot editor loads the debug variant).
scons target=template_debug platform=windows arch=x86_64

# 2. Copy the output into the demo project.
cp project/addons/godot-desktop-capture/bin/libgodot-desktop-capture.windows.debug.x86_64.dll \
   ../godot-charts/demo/addons/godot-desktop-capture/bin/
```

On Linux replace `.dll` with `.so` and `windows` with `linux`. After copying, reload the godot-charts project in the Godot editor (`Project → Reload Current Project`) — the updated extension loads immediately.

---

## Usage

### Basic — display desktop on a mesh

```gdscript
extends MeshInstance3D

func _ready() -> void:
    var capture := DesktopCaptureTexture.new()
    capture.monitor_index = 0     # primary monitor
    capture.capture_cursor = true
    capture.max_fps = 60

    # Assign before enabling so the material is ready when the first frame arrives.
    var mat := StandardMaterial3D.new()
    mat.albedo_texture = capture
    set_surface_override_material(0, mat)

    capture.capture_stopped.connect(_on_capture_stopped)
    capture.enabled = true

func _on_capture_stopped(reason: String) -> void:
    push_warning("Capture stopped: " + reason)
```

### Multi-monitor

```gdscript
var capture := DesktopCaptureTexture.new()
print("Monitors available: ", capture.get_monitor_count())

for i in capture.get_monitor_count():
    print("  Monitor %d: %s" % [i, capture.get_monitor_size(i)])

capture.monitor_index = 1  # switch to second monitor
capture.enabled = true
```

### Error handling

```gdscript
capture.capture_stopped.connect(func(reason: String) -> void:
    match reason:
        "permission_denied":
            push_warning("Screen capture permission was denied.")
        "device_lost":
            push_warning("GPU/display device lost — try re-enabling.")
        "monitor_index_out_of_range":
            push_error("monitor_index %d does not exist." % capture.monitor_index)
        _:
            push_warning("Capture stopped: " + reason)
)
```

---

## API reference

### Properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `enabled` | `bool` | `false` | Start (`true`) or stop (`false`) the capture loop |
| `monitor_index` | `int` | `0` | Zero-based index of the monitor to capture |
| `capture_cursor` | `bool` | `true` | Composite the OS mouse cursor onto each frame |
| `max_fps` | `int` | `60` | Capture rate cap (1–240 fps) |

`enabled` is always truthful: if `set enabled = true` but the backend fails to start, `enabled` reverts to `false` and `capture_stopped(reason)` is emitted.

### Methods

| Signature | Description |
|-----------|-------------|
| `get_monitor_count() -> int` | Number of monitors detected by the platform backend |
| `get_monitor_size(index: int) -> Vector2i` | Pixel dimensions of the monitor at `index` |

### Signals

| Signal | Description |
|--------|-------------|
| `capture_started` | Emitted once the backend initialises successfully |
| `capture_stopped(reason: String)` | Emitted when capture stops for any reason |
| `frame_updated` | Emitted after each new frame is written to the texture |

### `capture_stopped` reason values

| Value | Meaning |
|-------|---------|
| `"disabled"` | Caller set `enabled = false` |
| `"device_lost"` | GPU/display device lost and could not recover |
| `"permission_denied"` | OS security policy refused screen capture |
| `"missing_dependency"` | Required runtime library not found |
| `"monitor_index_out_of_range"` | `monitor_index >= get_monitor_count()` |
| `"unsupported_platform"` | No backend available on this platform |

---

## Launching the godot-charts demo on Windows with a Meta Quest 3

The [godot-charts](https://github.com/LabmarketAI/godot-charts) demo project includes a live desktop panel alongside 3D charts in a VR data room (`demo/scenes/main_vr.tscn`). These steps show how to run it tethered to a Windows PC.

### Prerequisites

| Item | Notes |
|------|-------|
| Windows 10/11 PC with a discrete GPU | GTX 1060 / RX 580 or better recommended |
| Godot 4.6 | [godot.org/download](https://godot.org/download) — standard (not .NET) build |
| Meta Quest 3 headset | Firmware up to date |
| Meta Quest Link app | [meta.com/quest/setup](https://www.meta.com/quest/setup) — installs the Link runtime and Air Link |
| USB-C cable **or** 5 GHz Wi-Fi router (for Air Link) | USB 3 cable gives the most stable connection |

### Step 1 — Connect the headset to your PC

**Option A — USB (Quest Link)**
1. Plug the headset into the PC with a USB-C cable.
2. Put on the headset — a prompt appears asking to **Allow access**. Accept it.
3. Select **Quest Link** from the headset's app library and launch it.

**Option B — Wireless (Air Link)**
1. In the headset go to **Settings → System → Quest Link** and enable Air Link.
2. Select your PC from the list and click **Launch**.

The Meta Quest Link app on the PC should show the headset as connected (green indicator in the top-left of the app).

### Step 2 — Set Meta as the active OpenXR runtime

Godot reads the system's default OpenXR runtime. The Meta Quest Link app registers itself as an OpenXR runtime, but you may need to activate it:

1. Open the **Meta Quest Link** app on your PC.
2. Go to **Settings → General**.
3. Under **OpenXR Runtime**, click **Set Meta Quest Link as Active**.

> If you also have SteamVR installed it may be set as the default instead. The button will read "SteamVR is your current OpenXR runtime — click to switch to Meta."

### Step 3 — Open the demo project in Godot

```
File → Open Project → navigate to godot-charts/demo/ → select project.godot
```

Let the editor import assets on first open (a few seconds). You do not need to enable any plugins manually — the `godot-desktop-capture` GDExtension loads automatically from its `.gdextension` file.

### Step 4 — Run the VR scene

1. In the **FileSystem** panel open `scenes/main_vr.tscn`.
2. Press **F5** (or the ▶ Run button) to launch.
3. The Godot window will appear on your monitor and the VR view will load in the headset.

In the headset you will see the data room with seven 3D charts and, on the north-west wall, the **Live Desktop** panel — a 4.8 × 2.7 m screen showing a live capture of your primary Windows monitor.

### Controls (in headset)

| Action | Input |
|--------|-------|
| Move | Left thumbstick |
| Snap-turn | Right thumbstick left/right |
| Teleport | Hold right trigger, aim at floor, release |
| Grab / interact | Grip buttons |

### Troubleshooting

**Black screen on the desktop panel**
- Confirm the Godot process is running in an interactive desktop session (screen must not be locked).
- Check the Godot Output panel for a `capture_stopped` reason. `device_lost` usually means DXGI lost access — unlock the screen or dismiss any UAC prompt and it recovers automatically.

**"OpenXR not initialised" / headset view not starting**
- Make sure Meta Quest Link is running on the PC *before* pressing Play in Godot.
- Confirm Meta is set as the active OpenXR runtime (Step 2).
- Restart the Meta Quest Link app if the headset was reconnected after it launched.

**Low frame rate or judder**
- USB connection is more reliable than Air Link for full-resolution rendering.
- Lower `max_fps` on the `DesktopCaptureTexture` resource (inspector on `DesktopPanel` node) from 30 to 15 to reduce CPU copy overhead.

---

## Requirements

### Windows

- Godot 4.1 or later
- Windows 8 or later
- The process must be running in an interactive desktop session. DXGI Desktop Duplication is a per-session API — it only works when the calling process has access to the visible desktop of the logged-in user. Common situations where capture will silently return black frames or fail with `permission_denied` / `device_lost`:
  - **Locked screen** (`Win+L` or unattended timeout) — the secure desktop is a separate session; `DuplicateOutput` returns `E_ACCESSDENIED`. The backend will attempt to recover automatically when the screen is unlocked (up to ~1 second of retries).
  - **Remote Desktop (RDP)** — the local console session is disconnected when an RDP session takes over. The RDP session itself has its own virtual desktop and *can* run DXGI capture, but only from within that session.
  - **Headless / no-display servers** — machines without a physical or virtual display adapter attached will have no outputs to enumerate; `get_monitor_count()` returns 0.
  - **Windows services and scheduled tasks** — these run in Session 0, which is isolated from all interactive user sessions. DXGI Desktop Duplication is not available in Session 0.
  - **UAC elevation prompt** — while the secure desktop is shown for a UAC dialog, capture of the interactive desktop is suspended. Frames will resume automatically once the prompt is dismissed.

### Linux

- Godot 4.1 or later
- A Wayland or X11 desktop with PipeWire (Ubuntu 22.04+, Fedora 34+, and most modern distributions)
- `xdg-desktop-portal` (any backend — `xdg-desktop-portal-gnome`, `xdg-desktop-portal-kde`, `xdg-desktop-portal-wlr`, etc.)
- On first use the portal shows a **screen-share permission dialog** — select the monitor to share and click **Share**. The choice is not remembered between sessions; the dialog appears each time capture is started.
