#!/usr/bin/env bash

set -uo pipefail

# =============================================================================
# Single-shot rerun: YCSB-F on 3T (tiered-indexing-zxj) only.
#
# Purpose: verify N5 fix (TwoTreeAdapter.hpp cold-tree memcpy bounded by
# runtime payload length).
#
# Config 100% mirrors run_3t_repro.sh's ycsb_f branch — same working_set,
# cxl_gib, dram_bp, warmup/measure, AND the same --vi_fremove=true flag that
# the surrounding 3T runs use. The reference PASS is bench_3t_repro_*/ycsb_f_*.
#
# Per-test 300s timeout: if the binary hangs (e.g. stuck in Load), it's killed
# automatically — no babysitting.
#
# Result goes to: results_rerun_ycsb_f_3T_<timestamp>/
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/frontend"
SSD_PATH="/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_comparison"
CXL_DAX_DEVICE="/dev/dax0.1"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="$SCRIPT_DIR/results_rerun_ycsb_f_3T_${TIMESTAMP}"
mkdir -p "$RESULT_DIR"

# -----------------------------------------------------------------------------
# Configuration (mirrors run_3t_repro.sh exactly)
# -----------------------------------------------------------------------------
WORKING_SET_GIB=2.0
PAYLOAD_SIZE_BYTES=100
CXL_GIB=2.0
ZIPF_THETA=0.99
DRAM_BP_BASELINE=0.5
WORKER_THREADS=8
PP_THREADS=1
CXL_PP_THREADS=1
WARMUP_LOOKUPS=20000000
MEASURE_LOOKUPS=30000000
PROGRESS_INTERVAL=5000000

PER_TEST_TIMEOUT=300

# -----------------------------------------------------------------------------
# Run
# -----------------------------------------------------------------------------
binary="$BUILD_DIR/experiment_1_ycsb_f"
result_file="$RESULT_DIR/ycsb_f_tiered-indexing-zxj.log"

if [[ ! -x "$binary" ]]; then
   echo "[ERROR] binary not found: $binary"
   echo "[ERROR] rebuild first: (cd $REPO_ROOT/build && make -j64 experiment_1_ycsb_f)"
   exit 1
fi

echo "============================================================"
echo "  Rerun YCSB-F on 3T (tiered-indexing-zxj) — N5 verification"
echo "  $(date '+%Y-%m-%d %H:%M:%S')"
echo "============================================================"
echo "  binary      = $binary"
echo "  result_file = $result_file"
echo "  config      = working_set=${WORKING_SET_GIB}GiB cxl=${CXL_GIB}GiB"
echo "                bp=${DRAM_BP_BASELINE}GiB rc=0.0GiB workers=${WORKER_THREADS}"
echo "                warmup=${WARMUP_LOOKUPS} measure=${MEASURE_LOOKUPS}"
echo "                theta=${ZIPF_THETA} timeout=${PER_TEST_TIMEOUT}s"
echo "============================================================"

start_ts=$(date +%s)
exit_code=0

timeout --signal=KILL "${PER_TEST_TIMEOUT}" "$binary" \
   --test_zipf_theta="$ZIPF_THETA" \
   --test_working_set_gib="$WORKING_SET_GIB" \
   --test_payload_size_bytes="$PAYLOAD_SIZE_BYTES" \
   --test_warmup_lookups="$WARMUP_LOOKUPS" \
   --test_measure_lookups="$MEASURE_LOOKUPS" \
   --test_progress_interval="$PROGRESS_INTERVAL" \
   --worker_threads="$WORKER_THREADS" \
   --vi=true --wal=true --trunc=true \
   --ssd_path="$SSD_PATH" \
   --test_admission_mode=tiered-indexing-zxj \
   --cxl_tiering_enabled=true \
   --cxl_gib="$CXL_GIB" \
   --cxl_dax_device_path="$CXL_DAX_DEVICE" \
   --pp_threads="$PP_THREADS" \
   --cxl_pp_threads="$CXL_PP_THREADS" \
   --dram_buffer_pool_gib="$DRAM_BP_BASELINE" \
   --dram_recordcache_gib=0.0 \
   --vi_fremove=true \
   2>&1 | tee "$result_file" || exit_code=$?

elapsed=$(( $(date +%s) - start_ts ))
elapsed_min=$(awk "BEGIN {printf \"%.2f\", $elapsed / 60.0}")

echo ""
echo "============================================================"
if [[ "$exit_code" -eq 137 ]]; then
   echo "  [HANG] killed after ${elapsed}s (timeout=${PER_TEST_TIMEOUT}s)"
   echo "  log:   $result_file"
   echo "  next:  tail -n 80 \"$result_file\""
elif [[ "$exit_code" -ne 0 ]]; then
   echo "  [FAIL] exit_code=$exit_code  elapsed=${elapsed}s (${elapsed_min} min)"
   echo "  log:   $result_file"
   echo "  next:  tail -n 80 \"$result_file\""
else
   if grep -q "Final Summary" "$result_file"; then
      echo "  [PASS] elapsed=${elapsed}s (${elapsed_min} min)"
      echo "  log:   $result_file"
   else
      echo "  [SUSPECT] exit_code=0 but no 'Final Summary' marker — log truncated?"
      echo "  log:      $result_file"
      echo "  next:     tail -n 40 \"$result_file\""
   fi
fi
echo "============================================================"

exit "$exit_code"
