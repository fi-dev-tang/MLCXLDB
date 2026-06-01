#!/usr/bin/env bash

set -euo pipefail
trap '' HUP

# =============================================================================
# Exp3: WriteThrough Scalability (cxl-recordcache-WT_0517)
#
# Workloads: A, F (WriteThrough advantage scenarios)
# Variants:  two_level, dram_ssd_unconstrained
# Threads:   4, 8, 16, 32, 64, 128
# WS=150 GiB, CXL=110 GiB, DRAM=18 GiB, cached_ratio=85.3%
#
# Total runs: 2 WL × 2 variants × 6 thread configs = 24
# =============================================================================

# -----------------------------------------------------------------------------
# Repo-specific
# -----------------------------------------------------------------------------
SSD_PATH="/data1/zhizhi.tyf/cxl_test_tmp/exp3_writeThrough_scalable"
CXL_DAX_DEVICE="/dev/dax0.4"

# -----------------------------------------------------------------------------
# Paths
# -----------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="/home/zhizhi.tyf/cxl-recordcache-WT_0517"
BUILD_DIR="$REPO_ROOT/build/frontend"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="$SCRIPT_DIR/run_${TIMESTAMP}_WriteThrough_scalability"
mkdir -p "$RESULT_DIR"
MASTER_LOG="$RESULT_DIR/master.log"

# -----------------------------------------------------------------------------
# Matrix
# -----------------------------------------------------------------------------
THREAD_COUNTS=(4 8 16 32 64 128)
THETA=0.90
WORKLOADS=(a f)
VARIANTS=(two_level dram_ssd_unconstrained)

# -----------------------------------------------------------------------------
# Memory config (fixed for all thread counts)
# Total = 128 GiB, CXL:DRAM = 6.1:1
# -----------------------------------------------------------------------------
WS_GIB=150.0
CXL_GIB=110.0
DRAM_TOTAL_GIB=18.0
# two_level: BP:RC = 1:5 (A/F are RC-friendly)
TWO_LEVEL_BP_GIB=3.0
TWO_LEVEL_RC_GIB=15.0
# dram_ssd_unconstrained: all memory as DRAM BP
UNCONSTRAINED_BP_GIB=128.0

# -----------------------------------------------------------------------------
# Lookup config
# -----------------------------------------------------------------------------
WARMUP_LOOKUPS=800000000
MEASURE_LOOKUPS=100000000

# -----------------------------------------------------------------------------
# Threading & misc (base values; worker_threads varies per run)
# -----------------------------------------------------------------------------
PP_THREADS=1
CXL_PP_THREADS=1
TWO_LEVEL_ADMISSION_THREADS=2
FORWARD_EPOCH_THREAD=1
SIEVE_EVICTION_THREAD=1
RECORD_CACHE_PROMOTE_THREAD=4

PAYLOAD_SIZE_BYTES=100
PROGRESS_INTERVAL=1000000
WARMUP_PROGRESS_INTERVAL=2000000
COOLDOWN_SECONDS=30
COOLDOWN_BETWEEN_THREADS=60

# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------
log_phase() {
   echo ""
   echo "============================================================"
   echo "$1"
   echo "============================================================"
}

get_theta_tuning_flags() {
   cat <<'EOF'
--skew_threshold_ratio=0.08
--uniform_threshold_ratio=0.45
--max_per_page_visits=8000
--max_global_requests_window=2000000
--trigger_visit_histogram_update_size=1000000
EOF
}

# -----------------------------------------------------------------------------
# Pre-flight checks
# -----------------------------------------------------------------------------
preflight() {
   if [[ ! -e "$CXL_DAX_DEVICE" ]]; then
      echo "[ERROR] dax device $CXL_DAX_DEVICE not found"
      return 1
   fi
   for wl in "${WORKLOADS[@]}"; do
      if [[ ! -x "$BUILD_DIR/experiment_1_ycsb_${wl}" ]]; then
         echo "[ERROR] binary not found or not executable: $BUILD_DIR/experiment_1_ycsb_${wl}"
         return 1
      fi
   done
   if command -v lsof >/dev/null 2>&1; then
      local stale
      stale="$(lsof -t "$CXL_DAX_DEVICE" 2>/dev/null || true)"
      if [[ -n "$stale" ]]; then
         echo "[ERROR] dax device $CXL_DAX_DEVICE busy (pids: $stale)"
         return 1
      fi
   fi
}

TOTAL_RUN=0
TOTAL_EXPERIMENTS=0

count_experiments() {
   for _t in "${THREAD_COUNTS[@]}"; do
      for _wl in "${WORKLOADS[@]}"; do
         for _v in "${VARIANTS[@]}"; do
            TOTAL_EXPERIMENTS=$((TOTAL_EXPERIMENTS + 1))
         done
      done
   done
}

# -----------------------------------------------------------------------------
# run_one: single (threads, wl, variant) execution
# -----------------------------------------------------------------------------
run_one() {
   local threads="$1" wl="$2" variant="$3"

   TOTAL_RUN=$((TOTAL_RUN + 1))
   local binary="$BUILD_DIR/experiment_1_ycsb_${wl}"
   local result_file="$RESULT_DIR/result_ycsb${wl}_${variant}_theta${THETA}_ws${WS_GIB}gib_t${threads}_${TIMESTAMP}.log"

   if [[ ! -x "$binary" ]]; then
      echo "[ERROR] binary not found: $binary"
      return 1
   fi

   local theta_flags=()
   while IFS= read -r line; do
      [[ -n "$line" ]] && theta_flags+=("$line")
   done < <(get_theta_tuning_flags)

   local admission_mode=""
   local extra_flags=()

   case "$variant" in
      two_level)
         admission_mode="two_level"
         extra_flags=(
            --cxl_tiering_enabled=true
            --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads="$PP_THREADS"
            --cxl_pp_threads="$CXL_PP_THREADS"
            --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS"
            --delay_admission_recordcache_threads_start=true
            --dram_buffer_pool_gib="$TWO_LEVEL_BP_GIB"
            --dram_recordcache_gib="$TWO_LEVEL_RC_GIB"
            --forward_epoch_thread="$FORWARD_EPOCH_THREAD"
            --sieve_eviction_thread="$SIEVE_EVICTION_THREAD"
            --record_cache_promote_thread="$RECORD_CACHE_PROMOTE_THREAD"
            "${theta_flags[@]}"
         )
         ;;
      dram_ssd_unconstrained)
         admission_mode="lru"
         extra_flags=(
            --cxl_tiering_enabled=false
            --dram_buffer_pool_gib="$UNCONSTRAINED_BP_GIB"
            --pp_threads="$PP_THREADS"
         )
         ;;
      *)
         echo "[ERROR] unknown variant: $variant"
         return 1
         ;;
   esac

   local common_flags=(
      --test_payload_size_bytes="$PAYLOAD_SIZE_BYTES"
      --worker_threads="$threads"
      --vi=true
      --test_warmup_lookups="$WARMUP_LOOKUPS"
      --test_measure_lookups="$MEASURE_LOOKUPS"
      --test_warmup_progress_interval="$WARMUP_PROGRESS_INTERVAL"
      --test_progress_interval="$PROGRESS_INTERVAL"
      --ssd_path="$SSD_PATH"
      --trunc=true
      --wal=true
   )

   echo ""
   echo "[RUN][$TOTAL_RUN/$TOTAL_EXPERIMENTS] threads=$threads wl=$wl variant=$variant theta=$THETA ws=${WS_GIB}G warmup=$WARMUP_LOOKUPS measure=$MEASURE_LOOKUPS"
   echo "[RUN] result -> $result_file"

   local wl_start_ts wl_end_ts wl_elapsed_sec wl_elapsed_min
   wl_start_ts=$(date +%s)
   local exit_code=0

   "$binary" \
      --test_admission_mode="$admission_mode" \
      --test_zipf_theta="$THETA" \
      --test_working_set_gib="$WS_GIB" \
      "${extra_flags[@]}" \
      "${common_flags[@]}" \
      2>&1 | tee "$result_file" || exit_code=$?

   wl_end_ts=$(date +%s)
   wl_elapsed_sec=$((wl_end_ts - wl_start_ts))
   wl_elapsed_min=$(awk "BEGIN {printf \"%.2f\", $wl_elapsed_sec / 60.0}")

   if [[ "$exit_code" -ne 0 ]]; then
      echo "[WARN] threads=$threads wl=$wl variant=$variant exit code $exit_code (continuing)"
   fi

   echo "[TIME] threads=$threads wl=$wl variant=$variant theta=$THETA elapsed: ${wl_elapsed_sec}s (${wl_elapsed_min} min)" \
      | tee -a "$result_file"

   echo "[INFO] cooldown ${COOLDOWN_SECONDS}s"
   sleep "$COOLDOWN_SECONDS"
}

# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------
main() {
   log_phase "Exp3 WriteThrough Scalability (theta=$THETA, WS=${WS_GIB} GiB, threads=${THREAD_COUNTS[*]})"
   echo "[INFO] timestamp                = ${TIMESTAMP}"
   echo "[INFO] result_dir               = ${RESULT_DIR}"
   echo "[INFO] repo                     = cxl-recordcache-WT_0517"
   echo "[INFO] binary_dir               = ${BUILD_DIR}"
   echo "[INFO] ssd_path                 = ${SSD_PATH}"
   echo "[INFO] cxl_dax_device           = ${CXL_DAX_DEVICE}"
   echo "[INFO] workloads                = ${WORKLOADS[*]}"
   echo "[INFO] variants                 = ${VARIANTS[*]}"
   echo "[INFO] memory                   = CXL=${CXL_GIB}G DRAM=${DRAM_TOTAL_GIB}G (total=128G)"
   echo "[INFO] two_level split          = BP=${TWO_LEVEL_BP_GIB}G RC=${TWO_LEVEL_RC_GIB}G"
   echo "[INFO] unconstrained            = BP=${UNCONSTRAINED_BP_GIB}G"

   preflight
   count_experiments
   echo "[INFO] total_experiments        = ${TOTAL_EXPERIMENTS}"

   local OVERALL_START
   OVERALL_START=$(date +%s)

   local i threads
   for ((i=0; i<${#THREAD_COUNTS[@]}; i++)); do
      threads="${THREAD_COUNTS[$i]}"
      log_phase "Threads=${threads}"
      for wl in "${WORKLOADS[@]}"; do
         for variant in "${VARIANTS[@]}"; do
            run_one "$threads" "$wl" "$variant"
         done
      done
      if (( i < ${#THREAD_COUNTS[@]} - 1 )); then
         echo "[INFO] inter-thread cooldown ${COOLDOWN_BETWEEN_THREADS}s"
         sleep "$COOLDOWN_BETWEEN_THREADS"
      fi
   done

   local OVERALL_END OVERALL_SEC OVERALL_HR
   OVERALL_END=$(date +%s)
   OVERALL_SEC=$((OVERALL_END - OVERALL_START))
   OVERALL_HR=$(awk "BEGIN {printf \"%.2f\", $OVERALL_SEC / 3600.0}")

   log_phase "EXP3 SCALABILITY COMPLETE"
   echo "[DONE] result_dir = ${RESULT_DIR}"
   echo "[DONE] total_run  = ${TOTAL_RUN}"
   echo "[TIME] total elapsed = ${OVERALL_SEC}s (${OVERALL_HR} hr)"
}

main 2>&1 | tee "$MASTER_LOG"
