#!/usr/bin/env bash

set -euo pipefail

# =============================================================================
# Quick smoke test: verify all 4 modes work with minimal data
#
# Runs YCSB-C (read-only, simplest) + YCSB-A (read+write) with small parameters
# for each mode. Takes ~5-10 minutes total.
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/frontend"
SSD_PATH="/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_comparison"
CXL_DAX_DEVICE="/dev/dax0.1"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="$SCRIPT_DIR/smoke_test_${TIMESTAMP}"
mkdir -p "$RESULT_DIR"

# Minimal parameters for quick smoke validation
WORKING_SET_GIB=0.1
CXL_GIB=0.5
DRAM_BP_TWO_LEVEL=0.3
DRAM_RC_TWO_LEVEL=0.2
DRAM_BP_BASELINE=0.5
WORKER_THREADS=2
WARMUP_LOOKUPS=200000
MEASURE_LOOKUPS=500000

MODES=(two_level bf-tree tiered-indexing-zxj hybried-tier-asplos2025)
COOLDOWN_SECONDS=3

log_info() { echo "[INFO] $1"; }

run_ycsb_smoke() {
   local wl="$1"
   local mode="$2"
   local binary="$BUILD_DIR/experiment_1_ycsb_${wl}"
   local result_file="$RESULT_DIR/smoke_ycsb_${wl}_${mode}.log"

   local mode_flags=()
   case "$mode" in
      two_level)
         mode_flags=(
            --test_admission_mode=two_level
            --cxl_tiering_enabled=true --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads=1 --cxl_pp_threads=1
            --two_level_admission_threads=1
            --delay_admission_recordcache_threads_start=true
            --dram_buffer_pool_gib="$DRAM_BP_TWO_LEVEL"
            --dram_recordcache_gib="$DRAM_RC_TWO_LEVEL"
            --forward_epoch_thread=1 --sieve_eviction_thread=1
            --record_cache_promote_thread=2
         )
         ;;
      bf-tree)
         mode_flags=(
            --test_admission_mode=bf-tree
            --cxl_tiering_enabled=true --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads=1 --cxl_pp_threads=1
            --dram_buffer_pool_gib="$DRAM_BP_BASELINE"
            --dram_recordcache_gib=0.0
         )
         ;;
      tiered-indexing-zxj)
         mode_flags=(
            --test_admission_mode=tiered-indexing-zxj
            --cxl_tiering_enabled=true --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads=1 --cxl_pp_threads=1
            --dram_buffer_pool_gib="$DRAM_BP_BASELINE"
            --dram_recordcache_gib=0.0
         )
         ;;
      hybried-tier-asplos2025)
         mode_flags=(
            --test_admission_mode=hybried-tier-asplos2025
            --cxl_tiering_enabled=true --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads=1 --cxl_pp_threads=1
            --two_level_admission_threads=1
            --delay_admission_recordcache_threads_start=true
            --dram_buffer_pool_gib="$DRAM_BP_BASELINE"
            --dram_recordcache_gib=0.0
         )
         ;;
   esac

   echo "[SMOKE] ycsb_${wl} mode=${mode}"

   local exit_code=0
   "$binary" \
      --test_zipf_theta=0.99 \
      --test_working_set_gib="$WORKING_SET_GIB" \
      --test_payload_size_bytes=100 \
      --test_warmup_lookups="$WARMUP_LOOKUPS" \
      --test_measure_lookups="$MEASURE_LOOKUPS" \
      --test_progress_interval=1000000 \
      --worker_threads="$WORKER_THREADS" \
      --vi=true --wal=true --trunc=true \
      --ssd_path="$SSD_PATH" \
      "${mode_flags[@]}" \
      2>&1 | tee "$result_file" || exit_code=$?

   if [[ "$exit_code" -ne 0 ]]; then
      echo "[FAIL] ycsb_${wl} mode=${mode} exit code $exit_code"
   else
      echo "[PASS] ycsb_${wl} mode=${mode}"
   fi
   sleep "$COOLDOWN_SECONDS"
}

run_tpcc_smoke() {
   local mode="$1"
   local binary="$BUILD_DIR/tpcc_compare_test"
   local result_file="$RESULT_DIR/smoke_tpcc_${mode}.log"

   local mode_flags=()
   case "$mode" in
      two_level)
         mode_flags=(
            --test_admission_mode=two_level
            --cxl_tiering_enabled=true --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads=1 --cxl_pp_threads=1
            --two_level_admission_threads=1
            --delay_admission_recordcache_threads_start=true
            --dram_buffer_pool_gib="$DRAM_BP_TWO_LEVEL"
            --dram_recordcache_gib="$DRAM_RC_TWO_LEVEL"
            --forward_epoch_thread=1 --sieve_eviction_thread=1
            --record_cache_promote_thread=2
         )
         ;;
      bf-tree)
         mode_flags=(
            --test_admission_mode=bf-tree
            --cxl_tiering_enabled=true --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads=1 --cxl_pp_threads=1
            --dram_buffer_pool_gib="$DRAM_BP_BASELINE"
            --dram_recordcache_gib=0.0
         )
         ;;
      tiered-indexing-zxj)
         mode_flags=(
            --test_admission_mode=tiered-indexing-zxj
            --cxl_tiering_enabled=true --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads=1 --cxl_pp_threads=1
            --dram_buffer_pool_gib="$DRAM_BP_BASELINE"
            --dram_recordcache_gib=0.0
         )
         ;;
      hybried-tier-asplos2025)
         mode_flags=(
            --test_admission_mode=hybried-tier-asplos2025
            --cxl_tiering_enabled=true --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads=1 --cxl_pp_threads=1
            --two_level_admission_threads=1
            --delay_admission_recordcache_threads_start=true
            --dram_buffer_pool_gib="$DRAM_BP_BASELINE"
            --dram_recordcache_gib=0.0
         )
         ;;
   esac

   echo "[SMOKE] tpcc mode=${mode}"

   local exit_code=0
   "$binary" \
      --tpcc_warehouse_count=1 \
      --test_warmup_seconds=5 \
      --test_measure_seconds=10 \
      --test_load_data=true \
      --worker_threads="$WORKER_THREADS" \
      --vi=true --wal=true --trunc=true \
      --ssd_path="$SSD_PATH" \
      "${mode_flags[@]}" \
      2>&1 | tee "$result_file" || exit_code=$?

   if [[ "$exit_code" -ne 0 ]]; then
      echo "[FAIL] tpcc mode=${mode} exit code $exit_code"
   else
      echo "[PASS] tpcc mode=${mode}"
   fi
   sleep "$COOLDOWN_SECONDS"
}

# --- Main ---
YCSB_WORKLOADS=(a b c d e f)

echo "============================================================"
echo "  SMOKE TEST: 4 modes x (ycsb_a/b/c/d/e/f + tpcc)"
echo "============================================================"
log_info "result_dir = $RESULT_DIR"

for wl in "${YCSB_WORKLOADS[@]}"; do
   for mode in "${MODES[@]}"; do
      run_ycsb_smoke "$wl" "$mode"
   done
done

for mode in "${MODES[@]}"; do
   run_tpcc_smoke "$mode"
done

echo ""
echo "============================================================"
echo "  SMOKE TEST COMPLETE"
echo "============================================================"
echo "Results:"
ls -1 "$RESULT_DIR"
