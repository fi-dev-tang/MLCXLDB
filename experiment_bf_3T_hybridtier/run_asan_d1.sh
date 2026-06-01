#!/usr/bin/env bash
# Run D1 two_level TPC-C under AddressSanitizer to catch the heap corruption
# that crashes worker_1 inside DramHotPageCandidates::ClearPromotedSlot right
# after stock-table creation.
#
# Prerequisite: bash build_asan.sh
#
# What ASAN will tell us (if the bug is heap corruption):
#   1. The faulting *read* site (same as gdb showed: ClearPromotedSlot in
#      hashtable.h:1649).
#   2. The *previous write* that corrupted the byte — file + line + stack.
#   3. The allocation site of the corrupted heap object.
#
# That triple (read / write / alloc) is exactly what is missing from the gdb
# log and is the single thing that will let us name the real culprit instead
# of guessing.
#
# Tuning:
#   - workload kept identical to debug_d1_under_gdb.sh / run_retry_pending.sh
#     D1 phase, so any difference in behaviour is from ASAN instrumentation
#     only, not from a different scenario.
#   - timeout=1800s: ASAN adds ~2-3x slowdown; previous unsanitized runs
#     crashed in <2 min, so 30 min is plenty.
#   - detect_leaks=0: this is a long-running benchmark; leak reports at exit
#     would drown the heap-corruption report we care about.
#   - abort_on_error=1: ASAN dumps stack and aborts immediately (instead of
#     trying to continue), so the log ends at the first finding.
#   - halt_on_error=1: stop after first error (no cascade).
#   - fast_unwind_on_malloc=0: full libgcc unwinder on malloc/free, so the
#     "allocated at" stack is meaningful. Worth the perf hit here.
#   - print_stacktrace=1: stacks for everything.
#   - check_initialization_order: DISABLED. We tried it first and ASAN aborted
#     during program startup on a known gflags SIOF (global_registry_lock_
#     read before construction in mutex.h:265). That site is in third-party
#     gflags, not related to the D1 heap corruption, and halt_on_error=1
#     means it kills the process before our workload runs. Turning these off
#     so ASAN proceeds to the real bug.
#
# Usage:  bash run_asan_d1.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build_asan"
BINARY="$BUILD_DIR/frontend/tpcc_compare_test"

if [[ ! -x "$BINARY" ]]; then
   echo "[run_asan_d1] ERROR: ASAN binary not found: $BINARY"
   echo "[run_asan_d1] Run 'bash build_asan.sh' first."
   exit 1
fi

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="$SCRIPT_DIR/asan_d1_${TIMESTAMP}"
mkdir -p "$RESULT_DIR"
OUT="$RESULT_DIR/asan_d1.log"
ASAN_LOG="$RESULT_DIR/asan_report"   # ASAN appends .<pid> automatically

# Make libasan / libstdc++ from the GCC 13.2 toolchain reachable at runtime.
GCC_PREFIX=/home/zhizhi.tyf/local
export LD_LIBRARY_PATH="$GCC_PREFIX/lib64:${LD_LIBRARY_PATH:-}"

# ASAN runtime options (see header comment for rationale of each).
export ASAN_OPTIONS="\
halt_on_error=1:\
abort_on_error=1:\
print_stacktrace=1:\
detect_leaks=0:\
detect_stack_use_after_return=1:\
fast_unwind_on_malloc=0:\
malloc_context_size=30:\
log_path=$ASAN_LOG"

export LSAN_OPTIONS="detect_leaks=0"

echo "[run_asan_d1] starting $(date '+%H:%M:%S')"      | tee "$OUT"
echo "[run_asan_d1] binary:     $BINARY"                | tee -a "$OUT"
echo "[run_asan_d1] result_dir: $RESULT_DIR"            | tee -a "$OUT"
echo "[run_asan_d1] asan_log:   $ASAN_LOG.<pid>"        | tee -a "$OUT"
echo "[run_asan_d1] ASAN_OPTIONS=$ASAN_OPTIONS"         | tee -a "$OUT"
echo ""                                                  | tee -a "$OUT"

cd "$RESULT_DIR"

# Identical D1 workload to debug_d1_under_gdb.sh, so we are diffing ONLY
# the ASAN instrumentation, not parameters.
timeout --signal=KILL 1800 "$BINARY" \
   --tpcc_warehouse_count=10 \
   --test_warmup_seconds=20 \
   --test_measure_seconds=40 \
   --test_load_data=true \
   --worker_threads=8 \
   --vi=true --wal=true --trunc=true \
   --ssd_path=/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_asan_tl_tpcc \
   --test_admission_mode=two_level \
   --cxl_tiering_enabled=true --cxl_gib=2.0 \
   --cxl_dax_device_path=/dev/dax0.1 \
   --pp_threads=1 --cxl_pp_threads=1 \
   --two_level_admission_threads=2 \
   --delay_admission_recordcache_threads_start=true \
   --dram_buffer_pool_gib=0.125 \
   --dram_recordcache_gib=0.375 \
   --forward_epoch_thread=1 \
   --sieve_eviction_thread=1 \
   --record_cache_promote_thread=4 \
   2>&1 | tee -a "$OUT"

EXIT=$?
echo ""                                                  | tee -a "$OUT"
echo "[run_asan_d1] exit=$EXIT  $(date '+%H:%M:%S')"     | tee -a "$OUT"

# ASAN log files are <log_path>.<pid>. Surface them.
shopt -s nullglob
ASAN_FILES=("${ASAN_LOG}".*)
if (( ${#ASAN_FILES[@]} > 0 )); then
   echo ""                                               | tee -a "$OUT"
   echo "===== ASAN reports ====="                       | tee -a "$OUT"
   for f in "${ASAN_FILES[@]}"; do
      echo ""                                            | tee -a "$OUT"
      echo "--- $f ---"                                  | tee -a "$OUT"
      cat "$f"                                           | tee -a "$OUT"
   done
else
   echo "[run_asan_d1] no separate ASAN report file produced — check $OUT for inline ==.== ASAN lines" \
        | tee -a "$OUT"
fi

echo ""                                                  | tee -a "$OUT"
echo "[run_asan_d1] result_dir: $RESULT_DIR"             | tee -a "$OUT"
ls -la "$RESULT_DIR"/                                    | tee -a "$OUT"
