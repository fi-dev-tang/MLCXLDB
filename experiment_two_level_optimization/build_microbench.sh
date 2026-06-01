#!/usr/bin/env bash
# Build the standalone microbench against the repo's vendored ankerl header.
# Uses GCC 13.2 from /home/zhizhi.tyf/local/ (system gcc is 10.2.1, missing
# C++20 features used by the project).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

GXX="${GXX:-/home/zhizhi.tyf/local/bin/g++}"
ANKERL_INCLUDE="$REPO_ROOT/build/vendor/unordered_dense/src/unordered_dense_src/include"

if [[ ! -x "$GXX" ]]; then
   echo "[ERROR] $GXX not found"
   exit 1
fi
if [[ ! -f "$ANKERL_INCLUDE/ankerl/unordered_dense.h" ]]; then
   echo "[ERROR] ankerl header not at $ANKERL_INCLUDE"
   exit 1
fi

OUT="$SCRIPT_DIR/microbench_hot_path"
SRC="$SCRIPT_DIR/microbench_hot_path.cpp"

echo "[build] $GXX -O2 -g -std=c++20 -pthread -I$ANKERL_INCLUDE $SRC -o $OUT"
"$GXX" -O2 -g -std=c++20 -pthread \
   -I"$ANKERL_INCLUDE" \
   "$SRC" -o "$OUT"

echo "[ok] $OUT"
echo ""
echo "Run with: taskset -c 0 $OUT"
