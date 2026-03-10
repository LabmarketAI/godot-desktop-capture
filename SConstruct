#!/usr/bin/env python
import os
from glob import glob
from pathlib import Path

env = SConscript("godot-cpp/SConstruct")

# SCons' MSVC auto-detection can replace the LIB/INCLUDE/PATH values that
# vcvarsall.bat already set up correctly.  Restoring them ensures that:
#   - LIB/INCLUDE  -> the right CRT and SDK headers/libs are found
#   - PATH         -> cl.exe comes from the same toolset as those headers,
#                     preventing STL1001 "unexpected compiler version" errors
#                     when the installed toolset is newer than SCons' detected one.
if env["platform"] == "windows":
    for evar in ("LIB", "INCLUDE", "PATH"):
        val = os.environ.get(evar, "")
        if val:
            env.PrependENVPath(evar, val)

# Add source files.
env.Append(CPPPATH=["src/"])
sources = Glob("src/*.cpp")

# Windows-specific: link against DXGI / D3D11 / WinRT (C++/WinRT WINRT_IMPL_* stubs).
# windowsapp.lib provides the WINRT_IMPL_* forwarding symbols used by C++/WinRT
# headers and works for both UWP and Win32 desktop apps.
if env["platform"] == "windows":
    env.Append(LIBS=["dxgi", "d3d11", "windowsapp", "user32"])
    env.Append(CPPDEFINES=["_WIN32_WINNT=0x0602"])
    # /EHsc is required by C++/WinRT try/catch in backend_wgc.cpp.
    env.Append(CXXFLAGS=["/EHsc"])

# Linux-specific: headers for PipeWire and D-Bus (runtime dlopen; no link-time dep)
# Build deps only: sudo apt-get install libpipewire-0.3-dev libdbus-1-dev
if env["platform"] == "linux":
    env.ParseConfig("pkg-config --cflags libpipewire-0.3 || true")
    env.ParseConfig("pkg-config --cflags dbus-1 || true")

# Find gdextension path even if the directory or extension is renamed.
(extension_path,) = glob("project/addons/*/*.gdextension")

# Get the addon path (e.g. project/addons/godot-desktop-capture).
addon_path = Path(extension_path).parent

# Get the project name from the gdextension file (e.g. godot-desktop-capture).
project_name = Path(extension_path).stem

scons_cache_path = os.environ.get("SCONS_CACHE")
if scons_cache_path is not None:
    CacheDir(scons_cache_path)
    print("Scons cache enabled... (path: '" + scons_cache_path + "')")

# Create the library target.
debug_or_release = "release" if env["target"] == "template_release" else "debug"
if env["platform"] == "macos":
    library = env.SharedLibrary(
        "{0}/bin/lib{1}.{2}.{3}.framework/{1}.{2}.{3}".format(
            addon_path, project_name, env["platform"], debug_or_release
        ),
        source=sources,
    )
else:
    library = env.SharedLibrary(
        "{}/bin/lib{}.{}.{}.{}{}".format(
            addon_path,
            project_name,
            env["platform"],
            debug_or_release,
            env["arch"],
            env["SHLIBSUFFIX"],
        ),
        source=sources,
    )

Default(library)
