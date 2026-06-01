#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="/home/zhizhi.tyf/cxl-recordcache-dev"
BUILD_DIR="$REPO_ROOT/build"

export PATH="/home/zhizhi.tyf/local/bin:$PATH"
export LD_LIBRARY_PATH="/home/zhizhi.tyf/local/lib64:${LD_LIBRARY_PATH:-}"
export CC="/home/zhizhi.tyf/local/bin/gcc"
export CXX="/home/zhizhi.tyf/local/bin/g++"

cd "$BUILD_DIR"
cmake .. -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX"
make -j$(nproc) exp6_convergence_test

echo "[DONE] exp6_convergence_test built at: $BUILD_DIR/frontend/exp6_convergence_test"
