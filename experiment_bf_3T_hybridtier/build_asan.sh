#!/usr/bin/env bash
# Build tpcc_compare_test with AddressSanitizer to catch the D1 two_level
# heap corruption that segfaults inside DramHotPageCandidates::ClearPromotedSlot.
#
# Output binary: $REPO_ROOT/build_asan/frontend/tpcc_compare_test
#
# Notes:
#   - Uses GCC 13.2 from /home/zhizhi.tyf/local/ (project toolchain).
#   - -O1 instead of -O3 so ASAN reports give usable line numbers but the run
#     still finishes load-data in reasonable time.
#   - -fno-omit-frame-pointer + -g3 are kept (CMakeLists already adds these,
#     repeating is harmless).
#   - libstdc++ stays static (CMakeLists hard-codes -static-libstdc++). ASAN
#     only intercepts libc malloc/free + operator new/delete weak symbols, so
#     static libstdc++ is fine. Static libasan would NOT be — we link dynamic.
#   - Build dir is build_asan/ to keep the optimized release build untouched.
#
# Usage:  bash build_asan.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build_asan"

GCC_PREFIX=/home/zhizhi.tyf/local
export CC="$GCC_PREFIX/bin/gcc"
export CXX="$GCC_PREFIX/bin/g++"

# Make the linker find libasan.so.8 at runtime without touching system ldconfig.
export LD_LIBRARY_PATH="$GCC_PREFIX/lib64:${LD_LIBRARY_PATH:-}"

# ASAN flags. -fsanitize=address turns on heap/stack/global red-zones.
# detect_stack_use_after_return needs both compile + runtime flag.
ASAN_CXX="-fsanitize=address -fno-omit-frame-pointer -O1 -g3 -fno-optimize-sibling-calls"
ASAN_LD="-fsanitize=address -Wl,-rpath,$GCC_PREFIX/lib64"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "[build_asan] CC=$CC"
echo "[build_asan] CXX=$CXX"
echo "[build_asan] $($CXX --version | head -1)"
echo "[build_asan] BUILD_DIR=$BUILD_DIR"
echo "[build_asan] cmake configure..."

# CMAKE_BUILD_TYPE=RelWithDebInfo so CMakeLists' -O3 path is not taken; we
# control -O via CMAKE_CXX_FLAGS instead. RelWithDebInfo defaults to -O2 -g
# but CMakeLists appends -g3, and our -O1 (last on the cmdline) wins.
cmake .. \
   -DCMAKE_BUILD_TYPE=RelWithDebInfo \
   -DCMAKE_C_COMPILER="$CC" \
   -DCMAKE_CXX_COMPILER="$CXX" \
   -DCMAKE_C_FLAGS="$ASAN_CXX" \
   -DCMAKE_CXX_FLAGS="$ASAN_CXX" \
   -DCMAKE_EXE_LINKER_FLAGS="$ASAN_LD"

echo "[build_asan] building tpcc_compare_test (this takes a while)..."
cmake --build . -j --target tpcc_compare_test

BIN="$BUILD_DIR/frontend/tpcc_compare_test"
echo ""
echo "[build_asan] done."
echo "[build_asan] binary: $BIN"
ls -la "$BIN"
file "$BIN" 2>/dev/null | head -1 || true
echo ""
echo "[build_asan] To run: bash $SCRIPT_DIR/run_asan_d1.sh"
