#!/usr/bin/env bash
# build_local.sh
#
# Builds the godot-desktop-capture debug .so using GCC/Clang and copies it into
# the sibling godot-charts demo project.
#
# Targets Fedora (dnf). Should also work on other distros with minor tweaks.
#
# Usage:
#   cd godot-desktop-capture
#   ./build_local.sh

set -euo pipefail

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

step()  { echo -e "\n==> $*"; }
ok()    { echo "    [OK]  $*"; }
warn()  { echo "    [!]   $*"; }

# Ask to install a missing package via dnf (requires sudo).
request_install() {
    local pkg="$1"
    warn "$pkg not found."
    read -r -p "    Install $pkg via dnf? [Y/n] " ans
    if [[ "${ans,,}" == n* ]]; then
        echo "    Skipping. Install $pkg manually and re-run this script."
        exit 1
    fi
    sudo dnf install -y "$pkg"
}

# ---------------------------------------------------------------------------
# 1. Git
# ---------------------------------------------------------------------------

step "Checking Git"
if ! command -v git &>/dev/null; then
    request_install git
fi
ok "$(git --version)"

# ---------------------------------------------------------------------------
# 2. Python 3
# ---------------------------------------------------------------------------

step "Checking Python 3"
PYTHON=""
for cmd in python3 python; do
    if command -v "$cmd" &>/dev/null && "$cmd" --version 2>&1 | grep -q "Python 3"; then
        PYTHON="$cmd"
        break
    fi
done

if [[ -z "$PYTHON" ]]; then
    request_install python3
    PYTHON=python3
fi
ok "$($PYTHON --version)"

# ---------------------------------------------------------------------------
# 3. SCons (installed as a Python package via requirements.txt)
# ---------------------------------------------------------------------------

step "Checking SCons"
if ! "$PYTHON" -c "import SCons" 2>/dev/null; then
    warn "SCons not installed. Installing from requirements.txt..."
    "$PYTHON" -m pip install -r requirements.txt --quiet
    if ! "$PYTHON" -c "import SCons" 2>/dev/null; then
        echo "SCons install failed. Run manually: $PYTHON -m pip install -r requirements.txt"
        exit 1
    fi
fi
ok "$("$PYTHON" -m SCons --version 2>&1 | head -1)"

# ---------------------------------------------------------------------------
# 4. C++ compiler (g++ or clang++)
# ---------------------------------------------------------------------------

step "Checking C++ compiler"
if ! command -v g++ &>/dev/null && ! command -v clang++ &>/dev/null; then
    request_install gcc-c++
fi
if command -v g++ &>/dev/null; then
    ok "$(g++ --version | head -1)"
else
    ok "$(clang++ --version | head -1)"
fi

# ---------------------------------------------------------------------------
# 5. Build dependencies: PipeWire and D-Bus headers
# ---------------------------------------------------------------------------

step "Checking PipeWire dev headers (pipewire-devel)"
if ! pkg-config --exists libpipewire-0.3 2>/dev/null; then
    request_install pipewire-devel
fi
ok "libpipewire-0.3 $(pkg-config --modversion libpipewire-0.3)"

step "Checking D-Bus dev headers (dbus-devel)"
if ! pkg-config --exists dbus-1 2>/dev/null; then
    request_install dbus-devel
fi
ok "dbus-1 $(pkg-config --modversion dbus-1)"

# ---------------------------------------------------------------------------
# 6. godot-cpp submodule
# ---------------------------------------------------------------------------

step "Checking godot-cpp submodule"
if [[ ! -f "godot-cpp/SConstruct" ]]; then
    warn "godot-cpp submodule not initialised. Running git submodule update..."
    git submodule update --init --recursive
fi
ok "godot-cpp present"

# ---------------------------------------------------------------------------
# 7. Build
# ---------------------------------------------------------------------------

step "Building debug .so (SCons)"
"$PYTHON" -m SCons target=template_debug platform=linux arch=x86_64

SO="project/addons/godot-desktop-capture/bin/libgodot-desktop-capture.linux.debug.x86_64.so"
if [[ ! -f "$SO" ]]; then
    echo "Build reported success but .so not found at: $SO"
    exit 1
fi
ok ".so built: $SO"

# ---------------------------------------------------------------------------
# 8. Optionally copy to godot-charts repo
# ---------------------------------------------------------------------------

step "Looking for godot-charts repo"

CHARTS_ROOT="$(dirname "$(pwd)")/godot-charts"
if [[ ! -d "$CHARTS_ROOT" ]]; then
    warn "godot-charts not found at $CHARTS_ROOT -- skipping copy."
    echo "    .so is at: $SO"
    exit 0
fi
ok "Found: $CHARTS_ROOT"

# Find bin dirs that host godot-desktop-capture inside godot-charts.
mapfile -t BIN_DIRS < <(find "$CHARTS_ROOT" -type d -path "*/addons/godot-desktop-capture/bin" 2>/dev/null)

if [[ ${#BIN_DIRS[@]} -eq 0 ]]; then
    # bin/ doesn't exist yet -- look for the addon dir to create it under.
    mapfile -t ADDON_DIRS < <(find "$CHARTS_ROOT" -type d -path "*/addons/godot-desktop-capture" 2>/dev/null)
    if [[ ${#ADDON_DIRS[@]} -gt 0 ]]; then
        BIN_DIRS=("${ADDON_DIRS[0]}/bin")
        warn "bin/ subfolder does not exist yet; it will be created."
    fi
fi

if [[ ${#BIN_DIRS[@]} -eq 0 ]]; then
    warn "Could not find an addons/godot-desktop-capture folder inside $CHARTS_ROOT."
    echo "    .so is at: $SO"
else
    for DEST in "${BIN_DIRS[@]}"; do
        mkdir -p "$DEST"
        cp "$SO" "$DEST/"
        ok "Copied to $DEST"
    done
    echo -e "\nDone. In Godot: Project -> Reload Current Project to pick up the new .so."
fi
