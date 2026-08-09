#!/usr/bin/env bash
# build_flavor.sh — Linux configure+build for compressonatorcli with the
# bc7enc_rdo integration. Produces a fully statically-linked binary:
# no libstdc++, no libgcc_s, no libc, no libGL — nothing in
# `ldd` at all. See NOTES.md "Phase 5 — Linux close-out" for how this
# recipe was arrived at.
#
# Companion to tools/win/build_cli_batch.ps1. Same three flavors
# (off / unbatched / batch), same flag intent, same build-dir naming
# (build_cli_off / build_cli / build_cli_batch).
#
# Usage:
#   tools/linux/build_flavor.sh [off|unbatched|batch]
# Default flavor: batch.
#
# Prereqs (checked below):
#   - gcc/g++ with static libc/libstdc++ available (glibc-static or
#     the distro's equivalent; on Arch this is bundled with base
#     gcc/glibc, no extra package)
#   - Linux ISPC v1.31.0 unpacked at tools/ispc/linux/bin/ispc
#     (relative to the repo root that contains compressonator/ and
#     bc7enc_rdo/ side by side)
#   - bc7enc_rdo checkout at ../bc7enc_rdo (side-by-side with the
#     compressonator/ tree)
#
# What the flags mean:
#   -DOPTION_CMP_OPENCV=OFF     — the CLI needs no OpenCV once
#                                 ssim.cpp is gated (Phase 4 part 4).
#                                 Turning it off removes 7 direct
#                                 NEEDED entries and roughly 200
#                                 transitive libs (Qt6, GStreamer,
#                                 FFmpeg, X11, Vulkan, ...).
#   -DOPTION_CMP_OPENGL=OFF     — the BC7 CPU encode path never
#                                 links libGL; the default-ON gate on
#                                 CLI builds is what pulled it in.
#   -DOPTION_CMP_QT=OFF         — GUI only; CLI never needs it.
#   -DOPTION_CMP_DIRECTX=OFF    — Windows-only anyway; explicit for
#                                 parity with the Windows script.
#   -DOPTION_BUILD_KTX2=OFF     — pulls in extra deps for a format
#                                 the CLI's BC7 path doesn't use.
#   -DOPTION_BUILD_BROTLIG=OFF  — Brotli-G is a GPU codec, not on the
#                                 BC7 CPU path.
#   -DOPTION_BUILD_EXR=OFF      — OpenEXR pulls in more transitive
#                                 deps than the CLI's format list
#                                 justifies. Turn on if a downstream
#                                 use actually needs .exr I/O.
#   -DOPTION_CMP_ETC=OFF        — Strip Ericsson ETCPack from the
#                                 binary. Not linkage-driven — driven
#                                 by ETCPack's SLA restricting use to
#                                 Khronos-standard-compression
#                                 purposes. Default is ON to preserve
#                                 stock Compressonator behavior; this
#                                 script sets it OFF because the BC7
#                                 pipeline never needs ETC codecs.
#   -DCMAKE_EXE_LINKER_FLAGS="-static"
#                               — full static link. Not
#                                 -static-libgcc/-static-libstdc++
#                                 (that intermediate step only
#                                 statified our own C++ code, but
#                                 the OpenCV .so entries dragged
#                                 libstdc++ back in transitively;
#                                 now that OpenCV is off there is no
#                                 obstacle to full -static).
#   -DCMAKE_POLICY_VERSION_MINIMUM=3.5
#                               — required by bc7enc_rdo (its
#                                 CMakeLists asks for a policy
#                                 version modern CMake removed).

set -euo pipefail

FLAVOR="${1:-batch}"
case "$FLAVOR" in
  off)       USE_RDO=OFF; USE_BATCH=OFF; BUILD_DIR=build_cli_off   ;;
  unbatched) USE_RDO=ON;  USE_BATCH=OFF; BUILD_DIR=build_cli       ;;
  batch)    USE_RDO=ON;  USE_BATCH=ON;  BUILD_DIR=build_cli_batch ;;
  *) echo "usage: $0 [off|unbatched|batch]" >&2; exit 2 ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd "$SCRIPT_DIR/../.." && pwd)"        # compressonator/ root
ROOT="$(cd "$SRC/.." && pwd)"                 # repo root (holds tools/, bc7enc_rdo/)
BUILD="$SRC/$BUILD_DIR"
ISPC="$ROOT/tools/ispc/linux/bin/ispc"
BC7ENC="$ROOT/bc7enc_rdo"

echo "Flavor     : $FLAVOR"
echo "RepoRoot   : $ROOT"
echo "Source     : $SRC"
echo "Build      : $BUILD"
echo "ISPC       : $ISPC"
echo "bc7enc_rdo : $BC7ENC"

if [[ "$USE_RDO" == "ON" ]]; then
  [[ -x "$ISPC" ]] || { echo "ISPC not executable at $ISPC" >&2; exit 1; }
  [[ -f "$BC7ENC/bc7e.ispc" ]] || { echo "bc7enc_rdo checkout missing at $BC7ENC (need bc7e.ispc)" >&2; exit 1; }
fi

# Clean build dir so stale cache entries can't shadow the flags below.
rm -rf "$BUILD"

CMAKE_ARGS=(
  -S "$SRC"
  -B "$BUILD"
  -G "Unix Makefiles"
  -DCMAKE_BUILD_TYPE=Release
  -DOPTION_ENABLE_ALL_APPS=OFF
  -DOPTION_BUILD_APPS_CMP_CLI=ON
  -DOPTION_BUILD_APPS_CMP_GUI=OFF
  -DOPTION_BUILD_CMP_SDK=OFF
  -DOPTION_BUILD_APPS_CMP_UNITTESTS=OFF
  -DOPTION_BUILD_INTERNAL_CMP_TEST=OFF
  -DOPTION_CMP_OPENCV=OFF
  -DOPTION_CMP_OPENGL=OFF
  -DOPTION_CMP_QT=OFF
  -DOPTION_CMP_DIRECTX=OFF
  -DOPTION_BUILD_KTX2=OFF
  -DOPTION_BUILD_BROTLIG=OFF
  -DOPTION_BUILD_EXR=OFF
  -DOPTION_CMP_ETC=OFF
  -DOPTION_CMP_USE_BC7ENC_RDO="$USE_RDO"
  -DOPTION_CMP_USE_BC7ENC_RDO_BATCH="$USE_BATCH"
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
  -DCMAKE_EXE_LINKER_FLAGS=-static
)

if [[ "$USE_RDO" == "ON" ]]; then
  CMAKE_ARGS+=( -DBC7ENC_RDO_ISPC="$ISPC" -DBC7ENC_RDO_DIR="$BC7ENC" )
fi

cmake "${CMAKE_ARGS[@]}"
cmake --build "$BUILD" --parallel "$(nproc)"

BIN="$BUILD/bin/compressonatorcli-bin"
if [[ -f "$BIN" ]]; then
  echo
  echo "BUILD OK: $BIN"
  # Confirm static — one line, no failure if `file` disagrees;
  # the ldd check below is the substantive one.
  file "$BIN" | sed 's/^/  file: /'
  # ldd on a statically linked binary exits non-zero on Linux
  # (it can't inspect what isn't there). Capture output separately
  # so `set -o pipefail` doesn't turn a correct-static result into
  # a false warning.
  ldd_out="$(ldd "$BIN" 2>&1 || true)"
  if grep -q "not a dynamic executable" <<<"$ldd_out"; then
    echo "  ldd : not a dynamic executable (fully static, as intended)"
  else
    echo "  WARNING: binary is not fully static — ldd output follows:"
    printf '%s\n' "$ldd_out" | sed 's/^/    /'
  fi
else
  echo "Build reported success but binary not at expected path: $BIN" >&2
  exit 1
fi
