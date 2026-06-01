#!/usr/bin/env bash

set -euo pipefail
trap '' HUP

# =============================================================================
# Exp4: Parameter Sensitivity (cxl-recordcache-dev, ReadOnly)
#
# One-at-a-time sweep of 4 admission control parameters.
# Only two_level variant, YCSB-B, WS=8 GiB, 8 threads.
#
# Parameters swept (others held at baseline):
#   record_cms_col_num             : 1024, 4096, 16384, [65536], 262144
#   trigger_visit_histogram_update : 200000, 500000, [1000000], 2000000, 5000000
#   skew_threshold_ratio           : 0.02, 0.05, [0.10], 0.20, 0.40
#   uniform_threshold_ratio        : 0.15, 0.30, [0.50], 0.70, 0.90
#
# Baseline run shared across parameters. Total unique runs = 17.
# =============================================================================

# -----------------------------------------------------------------------------
# Repo-specific
# -----------------------------------------------------------------------------
SSD_PATH="/data1/zhizhi.tyf/cxl_test_tmp/exp4_exp6_readonly"
CXL_DAX_DEVICE="/dev/dax0.5"

# -----------------------------------------------------------------------------
# Paths
# -----------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="/home/zhizhi.tyf/cxl-recordcache-dev"
BUILD_DIR="$REPO_ROOT/build/frontend"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="$SCRIPT_DIR/run_${TIMESTAMP}_exp4_sensitivity"
mkdir -p "$RESULT_DIR"
MASTER_LOG="$RESULT_DIR/master.log"

# -----------------------------------------------------------------------------
# Fixed config
# -----------------------------------------------------------------------------
WL=b
WS_GIB=8.0
CXL_GIB=5.0
BP_GIB=0.20
RC_GIB=1.00
THETA=0.90
WORKER_THREADS=8

WARMUP_LOOKUPS=200000000
MEASURE_LOOKUPS=100000000

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

# -----------------------------------------------------------------------------
# Baseline parameter values
# -----------------------------------------------------------------------------
BASE_RECORD_CMS_COL=65536
BASE_TRIGGER_UPDATE=1000000
BASE_SKEW_RATIO=0.10
BASE_UNIFORM_RATIO=0.50
BASE_MAX_PER_PAGE=8000
BASE_MAX_GLOBAL_REQ=2000000

# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------
log_phase() {
   echo ""
   echo "============================================================"
   echo "$1"
   echo "============================================================"
}

# -----------------------------------------------------------------------------
# Pre-flight checks
# -----------------------------------------------------------------------------
preflight() {
   if [[ ! -e "$CXL_DAX_DEVICE" ]]; then
      echo "[ERROR] dax device $CXL_DAX_DEVICE not found"
      return 1
   fi
   if [[ ! -x "$BUILD_DIR/experiment_1_ycsb_${WL}" ]]; then
      echo "[ERROR] binary not found: $BUILD_DIR/experiment_1_ycsb_${WL}"
      return 1
   fi
}

TOTAL_RUN=0
TOTAL_EXPERIMENTS=0

# -----------------------------------------------------------------------------
# run_one: single parameter configuration
# -----------------------------------------------------------------------------
run_one() {
   local label="$1"
   local record_cms_col="$2"
   local trigger_update="$3"
   local skew_ratio="$4"
   local uniform_ratio="$5"

   TOTAL_RUN=$((TOTAL_RUN + 1))
   local binary="$BUILD_DIR/experiment_1_ycsb_${WL}"
   local result_file="$RESULT_DIR/result_ycsb${WL}_${label}_${TIMESTAMP}.log"

   echo ""
   echo "[RUN][$TOTAL_RUN/$TOTAL_EXPERIMENTS] $label"
   echo "     record_cms_col=$record_cms_col trigger_update=$trigger_update skew=$skew_ratio uniform=$uniform_ratio"
   echo "[RUN] result -> $result_file"

   local wl_start_ts wl_end_ts wl_elapsed_sec wl_elapsed_min
   wl_start_ts=$(date +%s)
   local exit_code=0

   "$binary" \
      --test_admission_mode=two_level \
      --test_zipf_theta="$THETA" \
      --test_warmup_lookups="$WARMUP_LOOKUPS" \
      --test_measure_lookups="$MEASURE_LOOKUPS" \
      --test_working_set_gib="$WS_GIB" \
      --test_payload_size_bytes="$PAYLOAD_SIZE_BYTES" \
      --worker_threads="$WORKER_THREADS" \
      --vi=true \
      --ssd_path="$SSD_PATH" \
      --trunc=true \
      --wal=true \
      --cxl_tiering_enabled=true \
      --cxl_gib="$CXL_GIB" \
      --cxl_dax_device_path="$CXL_DAX_DEVICE" \
      --pp_threads="$PP_THREADS" \
      --cxl_pp_threads="$CXL_PP_THREADS" \
      --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS" \
      --delay_admission_recordcache_threads_start=true \
      --dram_buffer_pool_gib="$BP_GIB" \
      --dram_recordcache_gib="$RC_GIB" \
      --forward_epoch_thread="$FORWARD_EPOCH_THREAD" \
      --sieve_eviction_thread="$SIEVE_EVICTION_THREAD" \
      --record_cache_promote_thread="$RECORD_CACHE_PROMOTE_THREAD" \
      --test_warmup_progress_interval="$WARMUP_PROGRESS_INTERVAL" \
      --test_progress_interval="$PROGRESS_INTERVAL" \
      --record_cms_col_num="$record_cms_col" \
      --trigger_visit_histogram_update_size="$trigger_update" \
      --skew_threshold_ratio="$skew_ratio" \
      --uniform_threshold_ratio="$uniform_ratio" \
      --max_per_page_visits="$BASE_MAX_PER_PAGE" \
      --max_global_requests_window="$BASE_MAX_GLOBAL_REQ" \
      2>&1 | tee "$result_file" || exit_code=$?

   wl_end_ts=$(date +%s)
   wl_elapsed_sec=$((wl_end_ts - wl_start_ts))
   wl_elapsed_min=$(awk "BEGIN {printf \"%.2f\", $wl_elapsed_sec / 60.0}")

   if [[ "$exit_code" -ne 0 ]]; then
      echo "[WARN] $label exit code $exit_code (continuing)"
   fi

   echo "[TIME] $label elapsed: ${wl_elapsed_sec}s (${wl_elapsed_min} min)" \
      | tee -a "$result_file"

   echo "[INFO] cooldown ${COOLDOWN_SECONDS}s"
   sleep "$COOLDOWN_SECONDS"
}

# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------
main() {
   log_phase "Exp4 Parameter Sensitivity (YCSB-${WL}, WS=${WS_GIB}G, theta=$THETA)"
   echo "[INFO] timestamp     = ${TIMESTAMP}"
   echo "[INFO] result_dir    = ${RESULT_DIR}"
   echo "[INFO] repo          = cxl-recordcache-dev"
   echo "[INFO] binary        = ${BUILD_DIR}/experiment_1_ycsb_${WL}"
   echo "[INFO] ssd_path      = ${SSD_PATH}"
   echo "[INFO] cxl_device    = ${CXL_DAX_DEVICE}"

   preflight

   # -----------------------------------------------------------------------
   # Count total experiments: 1 baseline + 4 params × 4 non-baseline = 17
   # -----------------------------------------------------------------------
   TOTAL_EXPERIMENTS=17
   echo "[INFO] total_experiments = ${TOTAL_EXPERIMENTS}"

   local OVERALL_START
   OVERALL_START=$(date +%s)

   # -----------------------------------------------------------------------
   # 0. Baseline
   # -----------------------------------------------------------------------
   log_phase "Baseline"
   run_one "baseline" \
      "$BASE_RECORD_CMS_COL" "$BASE_TRIGGER_UPDATE" "$BASE_SKEW_RATIO" "$BASE_UNIFORM_RATIO"

   # -----------------------------------------------------------------------
   # 1. Sweep: record_cms_col_num
   # -----------------------------------------------------------------------
   log_phase "Sweep: record_cms_col_num"
   for val in 1024 4096 16384 262144; do
      run_one "cms_col_${val}" \
         "$val" "$BASE_TRIGGER_UPDATE" "$BASE_SKEW_RATIO" "$BASE_UNIFORM_RATIO"
   done

   # -----------------------------------------------------------------------
   # 2. Sweep: trigger_visit_histogram_update_size
   # -----------------------------------------------------------------------
   log_phase "Sweep: trigger_visit_histogram_update_size"
   for val in 200000 500000 2000000 5000000; do
      run_one "trigger_${val}" \
         "$BASE_RECORD_CMS_COL" "$val" "$BASE_SKEW_RATIO" "$BASE_UNIFORM_RATIO"
   done

   # -----------------------------------------------------------------------
   # 3. Sweep: skew_threshold_ratio
   # -----------------------------------------------------------------------
   log_phase "Sweep: skew_threshold_ratio"
   for val in 0.02 0.05 0.20 0.40; do
      run_one "skew_${val}" \
         "$BASE_RECORD_CMS_COL" "$BASE_TRIGGER_UPDATE" "$val" "$BASE_UNIFORM_RATIO"
   done

   # -----------------------------------------------------------------------
   # 4. Sweep: uniform_threshold_ratio
   # -----------------------------------------------------------------------
   log_phase "Sweep: uniform_threshold_ratio"
   for val in 0.15 0.30 0.70 0.90; do
      run_one "uniform_${val}" \
         "$BASE_RECORD_CMS_COL" "$BASE_TRIGGER_UPDATE" "$BASE_SKEW_RATIO" "$val"
   done

   local OVERALL_END OVERALL_SEC OVERALL_HR
   OVERALL_END=$(date +%s)
   OVERALL_SEC=$((OVERALL_END - OVERALL_START))
   OVERALL_HR=$(awk "BEGIN {printf \"%.2f\", $OVERALL_SEC / 3600.0}")

   log_phase "EXP4 SENSITIVITY COMPLETE"
   echo "[DONE] result_dir = ${RESULT_DIR}"
   echo "[DONE] total_run  = ${TOTAL_RUN}"
   echo "[TIME] total elapsed = ${OVERALL_SEC}s (${OVERALL_HR} hr)"
}

main 2>&1 | tee "$MASTER_LOG"
