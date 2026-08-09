# build_cli_batch.ps1 — Windows/MSVC configure+build for
# compressonatorcli with the bc7enc_rdo integration (batched path).
# Run from a "Developer PowerShell for VS 2022" (or 2019); the script
# does NOT bootstrap the VS environment itself.
#
# Mirrors the Linux build:
#   cmake -S compressonator -B build_cli_batch \
#         -DOPTION_BUILD_APPS_CMP_CLI=ON \
#         -DOPTION_BUILD_APPS_CMP_GUI=OFF \
#         -DOPTION_BUILD_CMP_SDK=OFF \
#         -DOPTION_BUILD_APPS_CMP_UNITTESTS=OFF \
#         -DOPTION_CMP_USE_BC7ENC_RDO=ON \
#         -DOPTION_CMP_USE_BC7ENC_RDO_BATCH=ON \
#         -DBC7ENC_RDO_ISPC=<tree>/tools/ispc/linux/bin/ispc \
#         -DBC7ENC_RDO_DIR=<tree>/bc7enc_rdo \
#         -DCMAKE_POLICY_VERSION_MINIMUM=3.5
#
# Windows-specific deltas from the Linux invocation:
#   - Generator: Visual Studio 17 2022 (or 2019). Multi-config, so
#     --config Release at build time, not configure time.
#   - Arch: x64.
#   - ISPC path: tools/ispc/windows/ispc-v1.31.0-windows/bin/ispc.exe.
#   - OPTION_CMP_OPENCV=OFF unless an OpenCV_DIR is available. The
#     Linux build had OpenCV on because pkg-config found it; on Windows
#     this needs to be explicit either way, and the CLI does not need
#     OpenCV for the BC7 encode/decode path this project targets.
#   - OPTION_CMP_OPENGL=OFF for the same reason (CLI doesn't need it
#     for BC7).
#   - See Phase 4 part 1 audit in NOTES.md for the CMakeLists issue
#     that will surface during configure/build: cmp_core/CMakeLists.txt
#     line 99 emits `/arch:AVX-512`, which MSVC rejects; the correct
#     flag is `/arch:AVX512` (no hyphen). Fix in place before running
#     this script or the CMP_Core_AVX512 target will fail to build.
#
# Usage:
#   cd <repo root that contains compressonator/ and bc7enc_rdo/ side by side>
#   .\compressonator\tools\win\build_cli_batch.ps1
#
# Overrides:
#   -RepoRoot   path containing compressonator/, bc7enc_rdo/, tools/
#   -BuildDir   where CMake writes build artifacts (default: build_cli_batch)
#   -Generator  CMake generator string
#   -Config     Debug|Release (default Release)

param(
    [ValidateSet("off","unbatched","batch")]
    [string]$Flavor    = "batch",
    # Three levels up: tools\win -> tools -> compressonator\ -> repo root.
    # This script lives inside the compressonator submodule but drives a
    # build whose inputs (bc7enc_rdo\, tools\ispc\) sit in the outer repo,
    # so RepoRoot is the outer root, not the submodule root. Mirrors
    # tools/linux/build_flavor.sh, which does the same walk.
    [string]$RepoRoot  = (Resolve-Path "$PSScriptRoot\..\..\.."),
    [string]$BuildDir  = "",
    [string]$Generator = "Visual Studio 17 2022",
    [string]$Arch      = "x64",
    [string]$Config    = "Release"
)

# Flavor → CMake options + default build-dir name (mirrors Linux tree:
# build_cli_off = stock, build_cli = per-block bc7e, build_cli_batch = batched).
$UseRdo   = ($Flavor -ne "off")
$UseBatch = ($Flavor -eq "batch")
if ($BuildDir -eq "") {
    switch ($Flavor) {
        "off"       { $BuildDir = "build_cli_off"   }
        "unbatched" { $BuildDir = "build_cli"       }
        "batch"     { $BuildDir = "build_cli_batch" }
    }
}

$ErrorActionPreference = "Stop"

$Src        = Join-Path $RepoRoot "compressonator"
$Build      = Join-Path $Src      $BuildDir
$IspcExe    = Join-Path $RepoRoot "tools\ispc\windows\ispc-v1.31.0-windows\bin\ispc.exe"
$Bc7EncDir  = Join-Path $RepoRoot "bc7enc_rdo"

Write-Host "RepoRoot   : $RepoRoot"
Write-Host "Source     : $Src"
Write-Host "Build      : $Build"
Write-Host "ISPC       : $IspcExe"
Write-Host "bc7enc_rdo : $Bc7EncDir"
Write-Host "Generator  : $Generator ($Arch)"

if ($UseRdo) {
    if (-not (Test-Path $IspcExe)) {
        throw "ISPC not found at $IspcExe. Unpack tools/ispc/windows/ispc-v1.31.0-windows.zip first."
    }
    if (-not (Test-Path (Join-Path $Bc7EncDir "bc7e.ispc"))) {
        throw "bc7enc_rdo checkout not found at $Bc7EncDir (need bc7e.ispc)."
    }
}

# ---- Configure ----
$rdoFlag   = if ($UseRdo)   { "ON" } else { "OFF" }
$batchFlag = if ($UseBatch) { "ON" } else { "OFF" }

$cmakeArgs = @(
    "-S", $Src,
    "-B", $Build,
    "-G", $Generator,
    "-A", $Arch,
    "-DOPTION_ENABLE_ALL_APPS=OFF",
    "-DOPTION_BUILD_APPS_CMP_CLI=ON",
    "-DOPTION_BUILD_APPS_CMP_GUI=OFF",
    "-DOPTION_BUILD_CMP_SDK=OFF",
    "-DOPTION_BUILD_APPS_CMP_UNITTESTS=OFF",
    "-DOPTION_BUILD_INTERNAL_CMP_TEST=OFF",
    "-DOPTION_CMP_OPENCV=OFF",
    "-DOPTION_CMP_OPENGL=OFF",
    "-DOPTION_CMP_QT=OFF",
    "-DOPTION_CMP_DIRECTX=OFF",
    "-DOPTION_BUILD_KTX2=OFF",
    "-DOPTION_BUILD_BROTLIG=OFF",
    "-DOPTION_BUILD_EXR=OFF",
    "-DOPTION_CMP_ETC=OFF",
    "-DOPTION_CMP_USE_BC7ENC_RDO=$rdoFlag",
    "-DOPTION_CMP_USE_BC7ENC_RDO_BATCH=$batchFlag",
    "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
)
if ($UseRdo) {
    $cmakeArgs += "-DBC7ENC_RDO_ISPC=$IspcExe"
    $cmakeArgs += "-DBC7ENC_RDO_DIR=$Bc7EncDir"
}

cmake @cmakeArgs

if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }

# ---- Build ----
cmake --build $Build --config $Config --parallel

if ($LASTEXITCODE -ne 0) { throw "cmake --build failed ($LASTEXITCODE)" }

$Bin = Join-Path $Build "bin\$Config\compressonatorcli.exe"
if (Test-Path $Bin) {
    Write-Host "`nBUILD OK: $Bin"
} else {
    Write-Warning "Build reported success but binary not at expected path: $Bin"
    Get-ChildItem -Recurse -Filter compressonatorcli-bin*.exe $Build | Select-Object FullName
}
