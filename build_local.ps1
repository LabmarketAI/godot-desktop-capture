# build_local.ps1
#
# Builds the godot-desktop-capture debug DLL using MSVC and copies it into
# the sibling godot-charts demo project.
#
# Run from any PowerShell window -- no Developer Command Prompt needed.
# Missing dependencies are detected and you are prompted to install them.
#
# Usage:
#   cd godot-desktop-capture
#   .\build_local.ps1

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

function Write-Step([string]$msg) {
    Write-Host "`n==> $msg" -ForegroundColor Cyan
}

function Write-Ok([string]$msg) {
    Write-Host "    [OK] $msg" -ForegroundColor Green
}

function Write-Warn([string]$msg) {
    Write-Host "    [!]  $msg" -ForegroundColor Yellow
}

function Request-Install([string]$name, [string]$wingetId) {
    Write-Warn "$name not found."
    $ans = Read-Host "    Install $name via winget? [Y/n]"
    if ($ans -match '^[Nn]') {
        Write-Host "    Skipping. Install $name manually and re-run this script." -ForegroundColor Yellow
        exit 1
    }
    winget install --id $wingetId -e --source winget
    # Refresh PATH for the current session.
    $machine = [System.Environment]::GetEnvironmentVariable("PATH", "Machine")
    $user    = [System.Environment]::GetEnvironmentVariable("PATH", "User")
    $env:PATH = "$machine;$user"
}

function Find-Python {
    foreach ($cmd in @("python", "python3")) {
        $p = Get-Command $cmd -ErrorAction SilentlyContinue
        if ($p) {
            $v = & $cmd --version 2>&1
            if ($v -match "Python 3") { return $cmd }
        }
    }
    return $null
}

function Test-SconsInstalled([string]$pythonCmd) {
    & $pythonCmd -c "import SCons" 2>$null
    return ($LASTEXITCODE -eq 0)
}

# ---------------------------------------------------------------------------
# 1. Git
# ---------------------------------------------------------------------------

Write-Step "Checking Git"
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Request-Install "Git" "Git.Git"
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        Write-Host "Git still not found. Restart PowerShell after the installer completes." -ForegroundColor Red
        exit 1
    }
}
Write-Ok (git --version)

Write-Step "Initialising repository submodules"
git submodule update --init --recursive
Write-Ok "Repository submodules initialised"

# ---------------------------------------------------------------------------
# 2. Python 3
# ---------------------------------------------------------------------------

Write-Step "Checking Python 3"
$python = Find-Python
if (-not $python) {
    Request-Install "Python 3" "Python.Python.3"
    $python = Find-Python
    if (-not $python) {
        Write-Host "Python 3 still not found. Restart PowerShell after the installer completes." -ForegroundColor Red
        exit 1
    }
}
Write-Ok (& $python --version)

# ---------------------------------------------------------------------------
# 3. SCons (installed as a Python package via requirements.txt)
# ---------------------------------------------------------------------------

Write-Step "Checking SCons"
if (-not (Test-SconsInstalled $python)) {
    Write-Warn "SCons not installed. Installing from requirements.txt..."
    & $python -m pip install -r requirements.txt --quiet
    if (-not (Test-SconsInstalled $python)) {
        Write-Host "SCons install failed. Run manually: $python -m pip install -r requirements.txt" -ForegroundColor Red
        exit 1
    }
}
$sconsVer = & $python -m SCons --version 2>&1 | Select-Object -First 1
Write-Ok $sconsVer

# ---------------------------------------------------------------------------
# 4. godot-cpp submodule
# ---------------------------------------------------------------------------

Write-Step "Checking godot-cpp submodule"
if (-not (Test-Path "godot-cpp\SConstruct")) {
    Write-Warn "godot-cpp submodule not initialised. Running git submodule update..."
    git submodule update --init --recursive
}
Write-Ok "godot-cpp present"

# ---------------------------------------------------------------------------
# 5. MSVC -- locate vcvarsall.bat via vswhere
# ---------------------------------------------------------------------------

Write-Step "Locating MSVC"
$vswhereCandidate = @(
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe",
    "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1

$vcvarsall = $null
if ($vswhereCandidate) {
    $vsPath = & $vswhereCandidate -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath 2>$null
    if ($vsPath) {
        $candidate = Join-Path $vsPath "VC\Auxiliary\Build\vcvarsall.bat"
        if (Test-Path $candidate) { $vcvarsall = $candidate }
    }
}

if (-not $vcvarsall) {
    Write-Warn "MSVC not found."
    $ans = Read-Host "    Install Visual Studio Build Tools (C++ workload) via winget? [Y/n]"
    if ($ans -notmatch '^[Nn]') {
        winget install --id Microsoft.VisualStudio.2022.BuildTools -e --source winget `
            --override "--quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
        Write-Host "`nVS Build Tools installation launched." -ForegroundColor Cyan
        Write-Host "Re-run this script once the installer finishes." -ForegroundColor Cyan
        exit 0
    }
    Write-Host "MSVC is required on Windows. Install it and re-run." -ForegroundColor Red
    exit 1
}
Write-Ok "vcvarsall: $vcvarsall"

# ---------------------------------------------------------------------------
# 5b. Determine the MSVC toolset version that has libcmt.lib
# ---------------------------------------------------------------------------
#
# Multiple MSVC toolchain versions can be installed side-by-side.
# vcvarsall.bat defaults to the *oldest* installed toolset, which may be
# missing libcmt.lib.  Find the latest version that actually has it and
# pass -vcvars_ver= so the linker gets the right LIB path.

$msvcBase = Join-Path $vsPath "VC\Tools\MSVC"
$vcvarsVer = $null
if (Test-Path $msvcBase) {
    $vcvarsVer = Get-ChildItem $msvcBase -Directory |
        Where-Object { Test-Path (Join-Path $_.FullName "lib\x64\libcmt.lib") } |
        Sort-Object Name -Descending |
        Select-Object -First 1 -ExpandProperty Name |
        ForEach-Object { ($_ -split '\.')[0..1] -join '.' }  # e.g. "14.44"
}

if ($vcvarsVer) {
    Write-Ok "MSVC toolset with libcmt.lib: $vcvarsVer"
} else {
    Write-Warn "Could not find an MSVC toolset with libcmt.lib -- build may fail at link step."
}

# ---------------------------------------------------------------------------
# 6 + 7. Load MSVC environment and build in a single cmd.exe session
# ---------------------------------------------------------------------------
#
# Running vcvarsall.bat and SCons in one cmd /c call guarantees that the
# LIB / INCLUDE / PATH vars set by vcvarsall are visible to the linker.
# -vcvars_ver= pins the toolset version so vcvarsall picks the one that
# actually has libcmt.lib rather than defaulting to the oldest installed.

Write-Step "Building debug DLL (vcvarsall + SCons in one cmd session)"

$vcvarsArgs = "x64"
if ($vcvarsVer) { $vcvarsArgs = "x64 -vcvars_ver=$vcvarsVer" }

$buildCmd = "`"$vcvarsall`" $vcvarsArgs && `"$python`" -m SCons target=template_debug platform=windows arch=x86_64"
cmd /c $buildCmd
if ($LASTEXITCODE -ne 0) {
    Write-Host "`nBuild failed (exit $LASTEXITCODE)." -ForegroundColor Red
    exit $LASTEXITCODE
}

$dll = "project\addons\godot-desktop-capture\bin\libgodot-desktop-capture.windows.debug.x86_64.dll"
if (-not (Test-Path $dll)) {
    Write-Host "Build reported success but DLL not found at: $dll" -ForegroundColor Red
    exit 1
}
Write-Ok "DLL built: $dll"

# ---------------------------------------------------------------------------
# 8. Optionally copy to godot-charts repo
# ---------------------------------------------------------------------------

Write-Step "Looking for godot-charts repo"

$chartsRoot = Resolve-Path "..\godot-charts" -ErrorAction SilentlyContinue
if (-not $chartsRoot) {
    Write-Warn "godot-charts not found at ..\godot-charts -- skipping copy."
    Write-Host "    DLL is at: $dll" -ForegroundColor Cyan
} else {
    Write-Ok "Found: $chartsRoot"

    Write-Step "Initialising XR demo dependencies in godot-charts"

    $chartsPath = $chartsRoot.Path
    $xrToolsPath = Join-Path $chartsPath "demo\addons\godot-xr-tools"
    $openxrVendorsPath = Join-Path $chartsPath "demo\addons\godot-openxr-vendors"

    if (Test-Path $xrToolsPath) {
        Write-Ok "XR Tools present: $xrToolsPath"
    } else {
        $xrToolsZip = Join-Path $chartsPath "demo\addons\godot-xr-tools.zip"
        if (Test-Path $xrToolsZip) {
            Write-Warn "XR Tools folder missing. Found zip at $xrToolsZip. Extract it to demo\\addons\\godot-xr-tools."
        } else {
            Write-Warn "XR Tools not found at demo\\addons\\godot-xr-tools. VR demo movement/interaction may be unavailable."
        }
    }

    if (Test-Path (Join-Path $chartsPath ".gitmodules")) {
        $hasOpenxrVendorsSubmodule = Select-String -Path (Join-Path $chartsPath ".gitmodules") -Pattern "demo/addons/godot-openxr-vendors" -Quiet
        if ($hasOpenxrVendorsSubmodule) {
            git -C $chartsPath submodule update --init --recursive demo/addons/godot-openxr-vendors
        }
    }

    if (Test-Path $openxrVendorsPath) {
        Write-Ok "OpenXR vendors present: $openxrVendorsPath"
    } else {
        Write-Warn "OpenXR vendors addon missing. Keyboard passthrough features will be unavailable."
    }

    # Search the repo for an addons subfolder that hosts godot-desktop-capture.
    $binDirs = Get-ChildItem $chartsRoot -Recurse -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match [regex]::Escape("addons\godot-desktop-capture\bin") }

    if (-not $binDirs) {
        # Folder doesn't exist yet; look for the parent addon directory to create bin/ inside it.
        $addonDirs = Get-ChildItem $chartsRoot -Recurse -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match [regex]::Escape("addons\godot-desktop-capture") }

        if ($addonDirs) {
            $binDirs = @($addonDirs[0].FullName + "\bin") | ForEach-Object { [PSCustomObject]@{ FullName = $_ } }
            Write-Warn "bin\ subfolder does not exist yet; it will be created if you choose to copy."
        }
    }

    if (-not $binDirs) {
        Write-Warn "Could not find an addons\godot-desktop-capture folder inside $chartsRoot."
        Write-Host "    DLL is at: $dll" -ForegroundColor Cyan
    } else {
        foreach ($binDir in $binDirs) {
            $dest = $binDir.FullName
            if (-not (Test-Path $dest)) { New-Item -ItemType Directory -Path $dest | Out-Null }
            Copy-Item $dll $dest -Force
            Write-Ok "Copied to $dest"
        }
        Write-Host "`nDone. In Godot: Project -> Reload Current Project to pick up the new DLL." -ForegroundColor Green
    }
}
