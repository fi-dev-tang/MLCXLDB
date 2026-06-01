#!/usr/bin/env bash

set -euo pipefail

# Ignore SIGHUP so the run survives SSH/terminal disconnects.
# Children (the YCSB binary) inherit this disposition.
trap '' HUP

# =============================================================================
# End-to-End Experiment 1 + Experiment 2 (single-theta, multi-WS sweep)
#   Repo: cxl-recordcache-WT_0517
#
# Purpose
#   Re-run end-to-end and ablation comparison under the new memory configuration
#   that satisfies  DRAM_tier < hot_data < CXL_tier < WS  for theta=0.90 across
#   WS = 4 / 8 / 16 GiB.
#
# Matrix
#   WS       = (4, 8, 16) GiB
#   theta    = 0.90 (single)
#   wl       = (b, c, a, d, f, e)            # E last; same theta as the others
#   variants = (two_level, page_only, lru, dram_ssd, dram_ssd_unconstrained)
#   total    = 3 ws × 6 wl × 5 variants      = 90 experiments
#
# Memory configuration (per WS, total invariant across (BP+RC) within a WS)
#   WS=4G  : DRAM_tier=0.6 GiB, CXL=2.5 GiB
#            tier1 (A/B/C/F): BP=0.10, RC=0.50
#            tier2 (D/E)    : BP=0.40, RC=0.20
#   WS=8G  : DRAM_tier=1.2 GiB, CXL=5.0 GiB
#            tier1 (A/B/C/F): BP=0.20, RC=1.00
#            tier2 (D/E)    : BP=0.80, RC=0.40
#   WS=16G : DRAM_tier=2.5 GiB, CXL=10.0 GiB
#            tier1 (A/B/C/F): BP=0.50, RC=2.00
#            tier2 (D/E)    : BP=1.50, RC=1.00
#   page_only / lru / dram_ssd : BP = DRAM_tier total
#   dram_ssd_unconstrained     : BP = DRAM_tier + CXL  (best-case upper bound)
#
# Lookups (Plan B: measure fixed at 100M, warmup scales with WS)
#   WARMUP_TIER1 = 100M (4G) / 200M (8G) / 400M (16G)   for A/B/C/F
#   WARMUP_TIER2 = 30M (constant across WS)             for D/E
#   MEASURE      = 100M
#
# Output
#   experiment_end_to_end/run_<TIMESTAMP>/
#     master.log                           # full stdout/stderr of this run
#     result_ycsb<wl>_<variant>_theta0.90_ws<ws>gib_<TIMESTAMP>.log   # per-run
#
# Estimated total time: ~28 hr (see 0518 estimation)
# =============================================================================

# -----------------------------------------------------------------------------
# Repo-specific: SSD and DAX device for cxl-recordcache-WT_0517
# -----------------------------------------------------------------------------
SSD_PATH="/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_ssd_wt"
CXL_DAX_DEVICE="/dev/dax0.3"

# -----------------------------------------------------------------------------
# Paths
# -----------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="/home/zhizhi.tyf/cxl-recordcache-WT_0517"
BUILD_DIR="$REPO_ROOT/build/frontend"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="$SCRIPT_DIR/run_${TIMESTAMP}"
mkdir -p "$RESULT_DIR"
MASTER_LOG="$RESULT_DIR/master.log"

# -----------------------------------------------------------------------------
# Matrix
# -----------------------------------------------------------------------------
WORKING_SETS=(4 8 16)
THETA=0.90
WORKLOADS=(b c a d f e)
VARIANTS=(two_level page_only lru dram_ssd dram_ssd_unconstrained)

# -----------------------------------------------------------------------------
# Lookup config (Plan B)
# -----------------------------------------------------------------------------
WARMUP_LOOKUPS_TIER2=30000000           # D/E   — RC has limited use, short warmup is enough
MEASURE_LOOKUPS=100000000               # fixed across WS for stable steady-state stats

# -----------------------------------------------------------------------------
# Threading & misc
# -----------------------------------------------------------------------------
WORKER_THREADS=8
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
COOLDOWN_BETWEEN_WS=60                  # let dax mapping fully release between WS

# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------
log_phase() {
   echo ""
   echo "============================================================"
   echo "$1"
   echo "============================================================"
}

is_tier2() {
   case "$1" in
      d|e) return 0 ;;
      *)   return 1 ;;
   esac
}

# Per-WS warmup for tier1 (A/B/C/F)
get_warmup_tier1() {
   case "$1" in
      4)  echo 100000000 ;;
      8)  echo 200000000 ;;
      16) echo 400000000 ;;
      *)  echo "[ERROR] unknown WS: $1" >&2; return 1 ;;
   esac
}

get_warmup_lookups() {
   local ws="$1" wl="$2"
   if is_tier2 "$wl"; then
      echo "$WARMUP_LOOKUPS_TIER2"
   else
      get_warmup_tier1 "$ws"
   fi
}

# Per-WS CXL size
get_cxl_gib() {
   case "$1" in
      4)  echo 2.5 ;;
      8)  echo 5.0 ;;
      16) echo 10.0 ;;
      *)  echo "[ERROR] unknown WS: $1" >&2; return 1 ;;
   esac
}

# Per-WS DRAM_tier total (used for page_only / lru / dram_ssd baseline BP)
get_dram_total() {
   case "$1" in
      4)  echo 0.6 ;;
      8)  echo 1.2 ;;
      16) echo 2.5 ;;
      *)  echo "[ERROR] unknown WS: $1" >&2; return 1 ;;
   esac
}

# dram_ssd_unconstrained BP = DRAM_tier + CXL  (upper-bound baseline)
get_dram_unconstrained() {
   awk "BEGIN {printf \"%.3f\", $(get_dram_total "$1") + $(get_cxl_gib "$1")}"
}

# Two-level BP per (WS, workload tier)
get_two_level_bp() {
   local ws="$1" wl="$2"
   if is_tier2 "$wl"; then
      case "$ws" in
         4)  echo 0.40 ;;
         8)  echo 0.80 ;;
         16) echo 1.50 ;;
      esac
   else
      case "$ws" in
         4)  echo 0.10 ;;
         8)  echo 0.20 ;;
         16) echo 0.50 ;;
      esac
   fi
}

# Two-level RC per (WS, workload tier)
get_two_level_rc() {
   local ws="$1" wl="$2"
   if is_tier2 "$wl"; then
      case "$ws" in
         4)  echo 0.20 ;;
         8)  echo 0.40 ;;
         16) echo 1.00 ;;
      esac
   else
      case "$ws" in
         4)  echo 0.50 ;;
         8)  echo 1.00 ;;
         16) echo 2.00 ;;
      esac
   fi
}

# Theta tuning (only theta=0.90 needed in this single-theta sweep)
get_theta_tuning_flags() {
   case "$1" in
      0.90)
         cat <<'EOF'
--skew_threshold_ratio=0.08
--uniform_threshold_ratio=0.45
--max_per_page_visits=8000
--max_global_requests_window=2000000
--trigger_visit_histogram_update_size=1000000
EOF
         ;;
      *)
         echo "[ERROR] unknown theta: $1" >&2
         return 1
         ;;
   esac
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
   # Only check via lsof on THIS dax device — pgrep would false-flag the other
   # repo running in parallel against a different dax device.
   if command -v lsof >/dev/null 2>&1; then
      local stale
      stale="$(lsof -t "$CXL_DAX_DEVICE" 2>/dev/null || true)"
      if [[ -n "$stale" ]]; then
         echo "[ERROR] dax device $CXL_DAX_DEVICE busy (pids: $stale)"
         echo "[HINT]  another YCSB process is still alive — kill it before retry"
         return 1
      fi
   fi
}

# -----------------------------------------------------------------------------
# Common flags shared by every run (not WS-specific)
# -----------------------------------------------------------------------------
COMMON_FLAGS_FIXED=(
   --test_payload_size_bytes="$PAYLOAD_SIZE_BYTES"
   --worker_threads="$WORKER_THREADS"
   --vi=true
   --test_measure_lookups="$MEASURE_LOOKUPS"
   --test_warmup_progress_interval="$WARMUP_PROGRESS_INTERVAL"
   --test_progress_interval="$PROGRESS_INTERVAL"
   --ssd_path="$SSD_PATH"
   --trunc=true
   --wal=true
)

TOTAL_EXPERIMENTS=$(( ${#WORKING_SETS[@]} * ${#WORKLOADS[@]} * ${#VARIANTS[@]} ))
CURRENT_EXPERIMENT=0

# -----------------------------------------------------------------------------
# run_one: single (ws, wl, variant) execution at THETA=0.90
# -----------------------------------------------------------------------------
run_one() {
   local ws="$1" wl="$2" variant="$3"
   local theta="$THETA"

   CURRENT_EXPERIMENT=$((CURRENT_EXPERIMENT + 1))
   local binary="$BUILD_DIR/experiment_1_ycsb_${wl}"
   local result_file="$RESULT_DIR/result_ycsb${wl}_${variant}_theta${theta}_ws${ws}gib_${TIMESTAMP}.log"

   if [[ ! -x "$binary" ]]; then
      echo "[ERROR] binary not found: $binary"
      return 1
   fi

   local theta_flags=()
   while IFS= read -r line; do
      [[ -n "$line" ]] && theta_flags+=("$line")
   done < <(get_theta_tuning_flags "$theta")

   local warmup_lookups
   warmup_lookups="$(get_warmup_lookups "$ws" "$wl")"

   local cxl_gib dram_total dram_unc ws_float
   cxl_gib="$(get_cxl_gib "$ws")"
   dram_total="$(get_dram_total "$ws")"
   dram_unc="$(get_dram_unconstrained "$ws")"
   ws_float=$(awk "BEGIN {printf \"%.1f\", $ws}")

   local admission_mode=""
   local extra_flags=()

   case "$variant" in
      two_level)
         admission_mode="two_level"
         local bp rc
         bp="$(get_two_level_bp "$ws" "$wl")"
         rc="$(get_two_level_rc "$ws" "$wl")"
         extra_flags=(
            --cxl_tiering_enabled=true
            --cxl_gib="$cxl_gib"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads="$PP_THREADS"
            --cxl_pp_threads="$CXL_PP_THREADS"
            --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS"
            --delay_admission_recordcache_threads_start=true
            --dram_buffer_pool_gib="$bp"
            --dram_recordcache_gib="$rc"
            --forward_epoch_thread="$FORWARD_EPOCH_THREAD"
            --sieve_eviction_thread="$SIEVE_EVICTION_THREAD"
            --record_cache_promote_thread="$RECORD_CACHE_PROMOTE_THREAD"
            "${theta_flags[@]}"
         )
         ;;
      page_only)
         admission_mode="page_only"
         extra_flags=(
            --cxl_tiering_enabled=true
            --cxl_gib="$cxl_gib"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads="$PP_THREADS"
            --cxl_pp_threads="$CXL_PP_THREADS"
            --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS"
            --delay_admission_recordcache_threads_start=true
            --dram_buffer_pool_gib="$dram_total"
            "${theta_flags[@]}"
         )
         ;;
      lru)
         admission_mode="lru"
         extra_flags=(
            --cxl_tiering_enabled=true
            --cxl_gib="$cxl_gib"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads="$PP_THREADS"
            --cxl_pp_threads="$CXL_PP_THREADS"
            --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS"
            --delay_admission_recordcache_threads_start=true
            --dram_buffer_pool_gib="$dram_total"
         )
         ;;
      dram_ssd)
         admission_mode="lru"
         extra_flags=(
            --cxl_tiering_enabled=false
            --dram_buffer_pool_gib="$dram_total"
            --pp_threads="$PP_THREADS"
         )
         ;;
      dram_ssd_unconstrained)
         admission_mode="lru"
         extra_flags=(
            --cxl_tiering_enabled=false
            --dram_buffer_pool_gib="$dram_unc"
            --pp_threads="$PP_THREADS"
         )
         ;;
      *)
         echo "[ERROR] unknown variant: $variant"
         return 1
         ;;
   esac

   echo ""
   echo "[RUN][$CURRENT_EXPERIMENT/$TOTAL_EXPERIMENTS] ws=${ws}G wl=$wl variant=$variant theta=$theta warmup=$warmup_lookups measure=$MEASURE_LOOKUPS"
   echo "[RUN] result -> $result_file"

   local wl_start_ts wl_end_ts wl_elapsed_sec wl_elapsed_min
   wl_start_ts=$(date +%s)
   local exit_code=0

   "$binary" \
      --test_admission_mode="$admission_mode" \
      --test_zipf_theta="$theta" \
      --test_warmup_lookups="$warmup_lookups" \
      --test_working_set_gib="$ws_float" \
      "${extra_flags[@]}" \
      "${COMMON_FLAGS_FIXED[@]}" \
      2>&1 | tee "$result_file" || exit_code=$?

   wl_end_ts=$(date +%s)
   wl_elapsed_sec=$((wl_end_ts - wl_start_ts))
   wl_elapsed_min=$(awk "BEGIN {printf \"%.2f\", $wl_elapsed_sec / 60.0}")

   if [[ "$exit_code" -ne 0 ]]; then
      echo "[WARN] ws=$ws wl=$wl variant=$variant exit code $exit_code (continuing)"
   fi

   echo "[TIME] ws=$ws wl=$wl variant=$variant theta=$theta elapsed: ${wl_elapsed_sec}s (${wl_elapsed_min} min)" \
      | tee -a "$result_file"

   echo "[INFO] cooldown ${COOLDOWN_SECONDS}s"
   sleep "$COOLDOWN_SECONDS"
}

# -----------------------------------------------------------------------------
# Main — wrapped so we can tee everything to master.log
# -----------------------------------------------------------------------------
main() {
   log_phase "End-to-End run start (theta=$THETA, WS=${WORKING_SETS[*]} GiB)"
   echo "[INFO] timestamp                = ${TIMESTAMP}"
   echo "[INFO] result_dir               = ${RESULT_DIR}"
   echo "[INFO] repo                     = cxl-recordcache-WT_0517"
   echo "[INFO] ssd_path                 = ${SSD_PATH}"
   echo "[INFO] cxl_dax_device           = ${CXL_DAX_DEVICE}"
   echo "[INFO] worker_threads           = ${WORKER_THREADS}"
   echo "[INFO] workloads                = ${WORKLOADS[*]}"
   echo "[INFO] variants                 = ${VARIANTS[*]}"
   echo "[INFO] WARMUP_LOOKUPS_TIER1     = 4G:100M  8G:200M  16G:400M  (A/B/C/F)"
   echo "[INFO] WARMUP_LOOKUPS_TIER2     = ${WARMUP_LOOKUPS_TIER2}  (D/E, fixed)"
   echo "[INFO] MEASURE_LOOKUPS          = ${MEASURE_LOOKUPS}"
   echo "[INFO] total_experiments        = ${TOTAL_EXPERIMENTS}"
   echo "[INFO] DRAM_tier (4G/8G/16G)    = 0.6 / 1.2 / 2.5  GiB"
   echo "[INFO] CXL       (4G/8G/16G)    = 2.5 / 5.0 / 10.0 GiB"
   echo "[INFO] dram_ssd_unconstrained   = 4G:$(get_dram_unconstrained 4)  8G:$(get_dram_unconstrained 8)  16G:$(get_dram_unconstrained 16)"

   preflight

   local OVERALL_START
   OVERALL_START=$(date +%s)

   local i ws
   for ((i=0; i<${#WORKING_SETS[@]}; i++)); do
      ws="${WORKING_SETS[$i]}"
      log_phase "WS=${ws} GiB  DRAM_tier=$(get_dram_total "$ws") GiB  CXL=$(get_cxl_gib "$ws") GiB  (6 wl × ${#VARIANTS[@]} variants)"
      for wl in "${WORKLOADS[@]}"; do
         for variant in "${VARIANTS[@]}"; do
            run_one "$ws" "$wl" "$variant"
         done
      done
      if (( i < ${#WORKING_SETS[@]} - 1 )); then
         echo "[INFO] inter-WS cooldown ${COOLDOWN_BETWEEN_WS}s (let dax mapping release)"
         sleep "$COOLDOWN_BETWEEN_WS"
      fi
   done

   local OVERALL_END OVERALL_SEC OVERALL_MIN OVERALL_HR
   OVERALL_END=$(date +%s)
   OVERALL_SEC=$((OVERALL_END - OVERALL_START))
   OVERALL_MIN=$(awk "BEGIN {printf \"%.1f\", $OVERALL_SEC / 60.0}")
   OVERALL_HR=$(awk "BEGIN {printf \"%.2f\", $OVERALL_SEC / 3600.0}")

   log_phase "All experiments completed"
   echo "[DONE] result_dir   = ${RESULT_DIR}"
   echo "[TIME] total elapsed = ${OVERALL_SEC}s (${OVERALL_MIN} min, ${OVERALL_HR} hr)"
}

main 2>&1 | tee "$MASTER_LOG"
