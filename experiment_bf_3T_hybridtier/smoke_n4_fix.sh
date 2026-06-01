#!/usr/bin/env bash
# =============================================================================
# Smoke test for N4 fix (delivery c_id uninit).
#
# Runs ONLY two_level tpcc 3 times. Each run:
#   - PASS criteria: no "EnsureFailed" in output, exit 0.
#   - Diagnostic: count "delivery skipped, order missing" WARNING — tells us
#     whether order.lookup1 actually returns NOT_FOUND under the workload.
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/frontend"
BINARY="$BUILD_DIR/tpcc_compare_test"
SSD_PATH="/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_n4_smoke"

TS=$(date +%Y%m%d_%H%M%S)
OUT_DIR="$SCRIPT_DIR/smoke_n4_${TS}"
mkdir -p "$OUT_DIR"

if [[ ! -x "$BINARY" ]]; then
   echo "[ERR] binary not found: $BINARY (rebuild first)" >&2
   exit 1
fi

PASS=0
FAIL=0

for i in 1 2 3; do
   echo ""
   echo "============================================================"
   echo "[INFO] attempt $i / 3"
   echo "============================================================"
   LOG="$OUT_DIR/run_${i}.log"

   rm -f "$SSD_PATH"

   exit_code=0
   "$BINARY" \
      --tpcc_warehouse_count=10 --test_warmup_seconds=20 --test_measure_seconds=40 \
      --test_load_data=true --worker_threads=8 --vi=true --wal=true --trunc=true \
      --ssd_path="$SSD_PATH" \
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
      2>&1 | tee "$LOG" || exit_code=$?

   skipped=$(grep -c "delivery skipped" "$LOG" 2>/dev/null || echo 0)
   ensure_failed=$(grep -c "EnsureFailed\|update1 NOT_FOUND" "$LOG" 2>/dev/null || echo 0)

   echo ""
   echo "[DIAG] attempt $i: exit=$exit_code  skipped=$skipped  ensure_failed=$ensure_failed"

   if (( exit_code == 0 && ensure_failed == 0 )); then
      PASS=$((PASS+1))
      echo "[PASS] attempt $i"
   else
      FAIL=$((FAIL+1))
      echo "[FAIL] attempt $i"
   fi
done

echo ""
echo "============================================================"
echo "RESULT: pass=$PASS fail=$FAIL / 3"
echo "============================================================"
echo "Logs in: $OUT_DIR"
ls -1 "$OUT_DIR"
echo ""
echo "Diagnostic queries:"
echo "  grep -c 'delivery skipped' $OUT_DIR/run_*.log"
echo "  grep -c 'EnsureFailed' $OUT_DIR/run_*.log"
echo "  grep -E 'TPS|Aborts' $OUT_DIR/run_*.log"
