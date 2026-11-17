#!/usr/bin/env bash
set -euo pipefail

# Build libdada_camfix.so and libdada_rtfix.so for AArch64 (Android API 34+)
# Usage: ANDROID_NDK_HOME=/path/to/ndk ./build.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="${OUT_DIR:-$SCRIPT_DIR/out}"

if [[ -z "${ANDROID_NDK_HOME:-}" ]]; then
  echo "Error: Set ANDROID_NDK_HOME to your Android NDK r26+ directory." >&2
  exit 1
fi

TOOLCHAIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
CXX="${CXX:-$TOOLCHAIN/aarch64-linux-android34-clang++}"
STRIP="${STRIP:-$TOOLCHAIN/llvm-strip}"

if [[ ! -x "$CXX" ]]; then
  echo "Error: Compiler not found at $CXX" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

COMMON_FLAGS=(
  -std=c++17
  -O2
  -fPIC
  -shared
  -Wall
  -Wextra
  -Wl,-z,max-page-size=16384
  -llog
  -ldl
)

"$CXX" "${COMMON_FLAGS[@]}" \
  -Wl,-soname,libdada_camfix.so \
  "$SCRIPT_DIR/dada_camfix.cpp" \
  -o "$OUT_DIR/libdada_camfix.so"

"$CXX" "${COMMON_FLAGS[@]}" \
  -Wl,-soname,libdada_rtfix.so \
  "$SCRIPT_DIR/dada_rtfix.cpp" \
  -o "$OUT_DIR/libdada_rtfix.so"

if [[ -x "$STRIP" ]]; then
  "$STRIP" --strip-unneeded "$OUT_DIR/libdada_camfix.so" "$OUT_DIR/libdada_rtfix.so"
fi

ls -lh "$OUT_DIR/libdada_camfix.so" "$OUT_DIR/libdada_rtfix.so"
