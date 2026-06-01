#!/usr/bin/env bash

set -euo pipefail
trap '' HUP

# =============================================================================
# Exp6: RecordCache Convergence Time Under Varying DRAM:CXL Ratios
#
# Measures time from RC cold-start to reaching EvictionWaterMark (0.95 slab
# usage) under different DRAM:CXL splits.
#
# Fixed:  WS=14G, total_memory=12G (85.7%), YCSB-C, two_level, theta=0.90
# Varied: DRAM:CXL ratio → 1:8, 1:6, 1:4, 1:3, 1:2
# BP:RC   = 1:5 (RC-friendly, YCSB-C)
#
# Shares DAX=dax0.5 and SSD=exp4_exp6_readonly with exp4.
# Total:  5 groups × ~10 min ≈ 50 min
# =============================================================================

# -----------------------------------------------------------------------------
# Repo paths
# -----------------------------------------------------------------------------
REPO_ROOT="/home/zhizhi.tyf/cxl-recordcache-dev"
BUILD_DIR="$REPO_ROOT/build/frontend"
BINARY="$BUILD_DIR/exp6_convergence_test"

SSD_PATH="/data1/zhizhi.tyf/cxl_test_tmp/exp4_exp6_readonly"
CXL_DAX_DEVICE="/dev/dax0.5"

# -----------------------------------------------------------------------------
# Output
# -----------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="$SCRIPT_DIR/run_${TIMESTAMP}_exp6_convergence"
mkdir -p "$RESULT_DIR"
MASTER_LOG="$RESULT_DIR/master.log"

# -----------------------------------------------------------------------------
# Fixed experiment parameters
# -----------------------------------------------------------------------------
WS_GIB=14.0
THETA=0.90
WORKER_THREADS=8
WARMUP_LOOKUPS=200000000
WARMUP_PROGRESS_INTERVAL=2000000
PAYLOAD_SIZE_BYTES=100

PP_THREADS=1
CXL_PP_THREADS=1
TWO_LEVEL_ADMISSION_THREADS=2
FORWARD_EPOCH_THREAD=1
SIEVE_EVICTION_THREAD=1
RECORD_CACHE_PROMOTE_THREAD=4

MONITOR_INTERVAL_MS=100
MAX_CONVERGENCE_SECS=1200
POST_CONVERGE_SECS=30
COOLDOWN_SECONDS=30

# -----------------------------------------------------------------------------
# Memory configurations (DRAM:CXL ratio sweep)
#
# total_memory = 12 GiB, WS = 14 GiB → cached ratio = 85.7%
# Within DRAM, BP:RC = 1:5
# -----------------------------------------------------------------------------
LABELS=(      "1to8"  "1to6"  "1to4"  "1to3"  "1to2"  )
DRAM_GIBS=(   1.33    1.71    2.40    3.00    4.00    )
CXL_GIBS=(    10.67   10.29   9.60    9.00    8.00    )
BP_GIBS=(     0.22    0.29    0.40    0.50    0.67    )
RC_GIBS=(     1.11    1.43    2.00    2.50    3.33    )

TOTAL_EXPERIMENTS=${#LABELS[@]}

# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------
log_phase() {
   echo ""
   echo "============================================================"
   echo "  $1"
   echo "  $(date '+%Y-%m-%d %H:%M:%S')"
   echo "============================================================"
}

log_info() { echo "[INFO] $(date '+%H:%M:%S') $1"; }

# -----------------------------------------------------------------------------
# Pre-flight
# -----------------------------------------------------------------------------
preflight() {
   if [[ ! -e "$CXL_DAX_DEVICE" ]]; then
      echo "[ERROR] DAX device $CXL_DAX_DEVICE not found"; exit 1
   fi
   if [[ ! -x "$BINARY" ]]; then
      echo "[ERROR] binary not found: $BINARY"
      echo "[HINT] run: bash build_exp6.sh"
      exit 1
   fi
}

# -----------------------------------------------------------------------------
# run_one
# -----------------------------------------------------------------------------
CURRENT=0
PASS_COUNT=0
FAIL_COUNT=0
declare -a FAILED_TESTS=()

run_one() {
   local idx="$1"
   local label="${LABELS[$idx]}"
   local dram="${DRAM_GIBS[$idx]}"
   local cxl="${CXL_GIBS[$idx]}"
   local bp="${BP_GIBS[$idx]}"
   local rc="${RC_GIBS[$idx]}"

   CURRENT=$((CURRENT + 1))

   local result_file="$RESULT_DIR/convergence_${label}.log"
   local csv_file="$RESULT_DIR/convergence_${label}.csv"

   rm -f "$SSD_PATH"

   log_phase "[$CURRENT/$TOTAL_EXPERIMENTS] DRAM:CXL=${label} (DRAM=${dram}G CXL=${cxl}G BP=${bp}G RC=${rc}G)"

   local start_ts exit_code=0
   start_ts=$(date +%s)

   "$BINARY" \
      --test_admission_mode=two_level \
      --test_zipf_theta="$THETA" \
      --test_working_set_gib="$WS_GIB" \
      --test_payload_size_bytes="$PAYLOAD_SIZE_BYTES" \
      --test_warmup_lookups="$WARMUP_LOOKUPS" \
      --test_warmup_progress_interval="$WARMUP_PROGRESS_INTERVAL" \
      --worker_threads="$WORKER_THREADS" \
      --vi=true --wal=true --trunc=true \
      --ssd_path="$SSD_PATH" \
      --cxl_tiering_enabled=true \
      --cxl_gib="$cxl" \
      --cxl_dax_device_path="$CXL_DAX_DEVICE" \
      --pp_threads="$PP_THREADS" \
      --cxl_pp_threads="$CXL_PP_THREADS" \
      --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS" \
      --delay_admission_recordcache_threads_start=true \
      --dram_buffer_pool_gib="$bp" \
      --dram_recordcache_gib="$rc" \
      --forward_epoch_thread="$FORWARD_EPOCH_THREAD" \
      --sieve_eviction_thread="$SIEVE_EVICTION_THREAD" \
      --record_cache_promote_thread="$RECORD_CACHE_PROMOTE_THREAD" \
      --test_convergence_csv="$csv_file" \
      --test_monitor_interval_ms="$MONITOR_INTERVAL_MS" \
      --test_max_convergence_secs="$MAX_CONVERGENCE_SECS" \
      --test_post_converge_secs="$POST_CONVERGE_SECS" \
      2>&1 | tee "$result_file" || exit_code=$?

   local elapsed=$(( $(date +%s) - start_ts ))

   if [[ "$exit_code" -ne 0 ]]; then
      echo "[FAIL] ${label} exit=$exit_code (${elapsed}s)"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("${label}:exit${exit_code}")
   else
      echo "[PASS] ${label} (${elapsed}s)"
      PASS_COUNT=$((PASS_COUNT + 1))
   fi

   echo "[TIME] ${label} elapsed=${elapsed}s" | tee -a "$result_file"

   # Extract key metrics for quick summary
   local twm cvm
   twm=$(grep -oP 'TIME_TO_WATERMARK_MS=\K[^ ]+' "$result_file" || echo "N/A")
   cvm=$(grep -oP 'CONVERGED_MS=\K[^ ]+' "$result_file" || echo "N/A")
   creason=$(grep -oP 'CONVERGE_REASON=\K.*' "$result_file" || echo "N/A")
   echo "[RESULT] ${label}: TIME_TO_WATERMARK_MS=${twm} CONVERGED_MS=${cvm} REASON=${creason}"

   sleep "$COOLDOWN_SECONDS"
}

# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------
main() {
   log_phase "Exp6: RecordCache Convergence Time (WS=${WS_GIB}G, theta=${THETA})"
   log_info "timestamp     = ${TIMESTAMP}"
   log_info "result_dir    = ${RESULT_DIR}"
   log_info "binary        = ${BINARY}"
   log_info "ssd_path      = ${SSD_PATH}"
   log_info "cxl_device    = ${CXL_DAX_DEVICE}"
   log_info "configurations = ${TOTAL_EXPERIMENTS}"

   for i in "${!LABELS[@]}"; do
      log_info "  ${LABELS[$i]}: DRAM=${DRAM_GIBS[$i]}G CXL=${CXL_GIBS[$i]}G BP=${BP_GIBS[$i]}G RC=${RC_GIBS[$i]}G"
   done

   preflight

   local OVERALL_START
   OVERALL_START=$(date +%s)

   for i in "${!LABELS[@]}"; do
      run_one "$i"
   done

   # --- Summary ---
   local OVERALL_SEC OVERALL_HR
   OVERALL_SEC=$(( $(date +%s) - OVERALL_START ))
   OVERALL_HR=$(awk "BEGIN {printf \"%.2f\", $OVERALL_SEC / 3600.0}")

   log_phase "EXP6 CONVERGENCE COMPLETE"
   log_info "total elapsed = ${OVERALL_SEC}s (${OVERALL_HR} hr)"
   log_info "pass=${PASS_COUNT} fail=${FAIL_COUNT} / ${TOTAL_EXPERIMENTS}"

   if [[ ${#FAILED_TESTS[@]} -gt 0 ]]; then
      echo ""
      echo "FAILED TESTS:"
      for t in "${FAILED_TESTS[@]}"; do
         echo "  - $t"
      done
   fi

   # --- Convergence summary table ---
   echo ""
   echo "=== CONVERGENCE SUMMARY ==="
   printf "%-8s  %-6s  %-6s  %-6s  %-6s  %-12s  %-12s  %s\n" \
      "Ratio" "DRAM" "CXL" "BP" "RC" "WatermarkMS" "ConvergedMS" "Reason"
   for i in "${!LABELS[@]}"; do
      local rfile="$RESULT_DIR/convergence_${LABELS[$i]}.log"
      local twm cvm creason
      twm=$(grep -oP 'TIME_TO_WATERMARK_MS=\K[^ ]+' "$rfile" 2>/dev/null || echo "N/A")
      cvm=$(grep -oP 'CONVERGED_MS=\K[^ ]+' "$rfile" 2>/dev/null || echo "N/A")
      creason=$(grep -oP 'CONVERGE_REASON=\K.*' "$rfile" 2>/dev/null || echo "N/A")
      printf "%-8s  %-6s  %-6s  %-6s  %-6s  %-12s  %-12s  %s\n" \
         "${LABELS[$i]}" "${DRAM_GIBS[$i]}" "${CXL_GIBS[$i]}" \
         "${BP_GIBS[$i]}" "${RC_GIBS[$i]}" "$twm" "$cvm" "$creason"
   done

   echo ""
   echo "Results in: $RESULT_DIR"
}

main 2>&1 | tee "$MASTER_LOG"
