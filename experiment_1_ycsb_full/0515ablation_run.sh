#!/usr/bin/env bash

set -euo pipefail

# =============================================================================
# Standalone ablation run: working_set = 16 GiB
#
# Purpose
#   Single-config ablation comparison at WS=16 GiB. Memory configuration is
#   inherited unchanged from run_ablation_ws4.sh (CXL=2.5 GiB, DRAM=0.625 GiB
#   total). The 16 GiB working set therefore stresses the SSD path: only
#   2.5/16 = 15.6% of records fit in CXL, the rest go to SSD.
#
# Workloads (single-theta sweep, all 6 workloads at theta=0.90):
#   for theta in (0.90):
#     for wl in (b, c, a, d, f):
#       run all 5 variants
#     E runs at SINGLE_THETA, which we override to 0.90 (vs ws=4's 0.95) so
#     every workload uses the same zipfian parameter.
#   Total = 6 wl × 1 theta × 5 variants = 30 experiments.
#
# Variants (per (wl, theta)):
#   1) two_level                (Full-System, MLCXLDB)
#   2) page_only                (Level-1 only, no RecordCache admission)
#   3) lru                      (no admission)
#   4) dram_ssd                 (memory-constrained baseline, no CXL)
#   5) dram_ssd_unconstrained   (best-case baseline, DRAM = 0.625 + CXL_GIB)
#
# ops sizing (single theta=0.90, all 6 workloads):
#   WARMUP_TIER1 = 100M (A/B/C/F — matches ws=4, converges L1 admission histogram)
#   WARMUP_TIER2 = 30M  (D/E      — matches ws=4 tier2)
#   MEASURE      = 10M  (short steady-state window)
#   Per-op time at ws=16, theta=0.90 (measured 5/15 run):
#     tier1 a/b/c/f ≈ 11 μs/op  → 110M ≈ 20 min/exp
#     tier2 d       ≈ 10 μs/op  →  40M ≈  7 min/exp
#     tier2 e (scan, estimated) ≈ 30-60 μs/op → 40M ≈ 20-40 min/exp
#   Total = 30 experiments ≈ 10 h (worst case ~13 h).
#
# Standalone use:
#   ./0515ablation_run.sh                         # writes to ./experiment_ws16_<ts>/
#   EXPERIMENT_ROOT=/path ./0515ablation_run.sh   # writes to /path/ws16/
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/frontend"
SSD_PATH="/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_comparison"
CXL_DAX_DEVICE="/dev/dax0.2"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
if [[ -n "${EXPERIMENT_ROOT:-}" ]]; then
   RESULT_DIR="${EXPERIMENT_ROOT}/ws16"
else
   RESULT_DIR="$SCRIPT_DIR/experiment_ws16_${TIMESTAMP}"
fi
mkdir -p "$RESULT_DIR"
MASTER_LOG="$RESULT_DIR/master.log"

# -----------------------------------------------------------------------------
# Workload / theta / variant matrix
# -----------------------------------------------------------------------------
WORKLOADS_FULL=(b c a d f)
WORKLOAD_SINGLE_THETA=(e)               # E runs at SINGLE_THETA only — overridden to 0.90 below to match the single-theta sweep
THETAS=(0.90)                           # single zipfian parameter for tomorrow-morning deadline
SINGLE_THETA=0.90                       # E runs at 0.90 (instead of the ws=4 default 0.95) so all 6 workloads share one theta
VARIANTS=(two_level page_only lru dram_ssd dram_ssd_unconstrained)

# -----------------------------------------------------------------------------
# Working-set / CXL / warmup config (this script: ws = 16 GiB)
#   Memory layout intentionally unchanged from ws=4: CXL=2.5, DRAM=0.625
# -----------------------------------------------------------------------------
WORKING_SET_GIB=16.0
PAYLOAD_SIZE_BYTES=100
CXL_GIB=2.5

WARMUP_LOOKUPS_TIER1=100000000          # A/B/C/F  (matches ws=4: 100M is where L1 admission histogram has empirically converged)
WARMUP_LOOKUPS_TIER2=30000000           # D/E     (matches ws=4 tier2: RC is small, longer warmup adds little)
MEASURE_LOOKUPS=10000000                # short measure window for steady-state throughput / latency snapshot

# -----------------------------------------------------------------------------
# Threading
# -----------------------------------------------------------------------------
WORKER_THREADS=8
PP_THREADS=1
CXL_PP_THREADS=1
TWO_LEVEL_ADMISSION_THREADS=2
FORWARD_EPOCH_THREAD=1
SIEVE_EVICTION_THREAD=1
RECORD_CACHE_PROMOTE_THREAD=4

PROGRESS_INTERVAL=1000000
WARMUP_PROGRESS_INTERVAL=2000000
COOLDOWN_SECONDS=30

# -----------------------------------------------------------------------------
# Variant memory config (GiB)  — identical to run_ablation_ws4.sh
# -----------------------------------------------------------------------------
DRAM_BP_TWO_LEVEL_T1=0.125
DRAM_RC_TWO_LEVEL_T1=0.500
DRAM_BP_TWO_LEVEL_T2=0.500
DRAM_RC_TWO_LEVEL_T2=0.125

DRAM_BP_PAGE_ONLY=0.625
DRAM_BP_LRU=0.625
DRAM_BP_DRAM_SSD=0.625
DRAM_BP_DRAM_SSD_UNCONSTRAINED=$(awk "BEGIN {printf \"%.3f\", 0.625 + ${CXL_GIB}}")

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

get_two_level_dram_bp() {
   if is_tier2 "$1"; then echo "$DRAM_BP_TWO_LEVEL_T2"; else echo "$DRAM_BP_TWO_LEVEL_T1"; fi
}

get_two_level_dram_rc() {
   if is_tier2 "$1"; then echo "$DRAM_RC_TWO_LEVEL_T2"; else echo "$DRAM_RC_TWO_LEVEL_T1"; fi
}

get_warmup_lookups() {
   if is_tier2 "$1"; then echo "$WARMUP_LOOKUPS_TIER2"; else echo "$WARMUP_LOOKUPS_TIER1"; fi
}

get_theta_tuning_flags() {
   local theta="$1"
   case "$theta" in
      0.90)
         cat <<'EOF'
--skew_threshold_ratio=0.08
--uniform_threshold_ratio=0.45
--max_per_page_visits=8000
--max_global_requests_window=2000000
--trigger_visit_histogram_update_size=1000000
EOF
         ;;
      0.95)
         cat <<'EOF'
--skew_threshold_ratio=0.10
--uniform_threshold_ratio=0.55
--max_per_page_visits=6000
--max_global_requests_window=1500000
--trigger_visit_histogram_update_size=1000000
EOF
         ;;
      0.99)
         cat <<'EOF'
--skew_threshold_ratio=0.12
--uniform_threshold_ratio=0.65
--max_per_page_visits=4000
--max_global_requests_window=1000000
--trigger_visit_histogram_update_size=1000000
EOF
         ;;
      *)
         echo "[ERROR] unknown theta: $theta" >&2
         return 1
         ;;
   esac
}

# Fail fast if any other YCSB binary still owns /dev/dax0.2
assert_no_running_ycsb() {
   local stale=""
   if command -v lsof >/dev/null 2>&1; then
      stale="$(lsof -t "$CXL_DAX_DEVICE" 2>/dev/null || true)"
   fi
   if [[ -z "$stale" ]]; then
      stale="$(pgrep -f 'experiment_1_ycsb_[abcdef]( |$)' || true)"
   fi
   if [[ -n "$stale" ]]; then
      echo "[ERROR] another YCSB process is still alive (pids: $stale)"
      echo "[ERROR] aborting to avoid /dev/dax0.2 contention"
      return 1
   fi
}

preflight() {
   if [[ ! -e "$CXL_DAX_DEVICE" ]]; then
      echo "[ERROR] dax device $CXL_DAX_DEVICE not found"
      return 1
   fi
   for wl in "${WORKLOADS_FULL[@]}" "${WORKLOAD_SINGLE_THETA[@]}"; do
      if [[ ! -x "$BUILD_DIR/experiment_1_ycsb_${wl}" ]]; then
         echo "[ERROR] binary not found or not executable: $BUILD_DIR/experiment_1_ycsb_${wl}"
         return 1
      fi
   done
   assert_no_running_ycsb
}

# -----------------------------------------------------------------------------
# Common flags shared by every run
# -----------------------------------------------------------------------------
COMMON_FLAGS=(
   --test_working_set_gib="$WORKING_SET_GIB"
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

TOTAL_EXPERIMENTS=$(( ${#WORKLOADS_FULL[@]} * ${#THETAS[@]} * ${#VARIANTS[@]}
                   + ${#WORKLOAD_SINGLE_THETA[@]} * 1            * ${#VARIANTS[@]} ))
CURRENT_EXPERIMENT=0

# -----------------------------------------------------------------------------
# run_one: single (wl, theta, variant) execution
# -----------------------------------------------------------------------------
run_one() {
   local wl="$1"
   local theta="$2"
   local variant="$3"

   CURRENT_EXPERIMENT=$((CURRENT_EXPERIMENT + 1))
   local binary="$BUILD_DIR/experiment_1_ycsb_${wl}"
   local result_file="$RESULT_DIR/result_ycsb${wl}_${variant}_theta${theta}_ws${WORKING_SET_GIB}gib_${TIMESTAMP}.log"

   if [[ ! -x "$binary" ]]; then
      echo "[ERROR] binary not found: $binary"
      return 1
   fi

   local theta_flags=()
   while IFS= read -r line; do
      [[ -n "$line" ]] && theta_flags+=("$line")
   done < <(get_theta_tuning_flags "$theta")

   local warmup_lookups
   warmup_lookups="$(get_warmup_lookups "$wl")"

   local admission_mode=""
   local extra_flags=()

   case "$variant" in
      two_level)
         admission_mode="two_level"
         local dram_bp dram_rc
         dram_bp="$(get_two_level_dram_bp "$wl")"
         dram_rc="$(get_two_level_dram_rc "$wl")"
         extra_flags=(
            --cxl_tiering_enabled=true
            --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads="$PP_THREADS"
            --cxl_pp_threads="$CXL_PP_THREADS"
            --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS"
            --delay_admission_recordcache_threads_start=true
            --dram_buffer_pool_gib="$dram_bp"
            --dram_recordcache_gib="$dram_rc"
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
            --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads="$PP_THREADS"
            --cxl_pp_threads="$CXL_PP_THREADS"
            --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS"
            --delay_admission_recordcache_threads_start=true
            --dram_buffer_pool_gib="$DRAM_BP_PAGE_ONLY"
            "${theta_flags[@]}"
         )
         ;;
      lru)
         admission_mode="lru"
         extra_flags=(
            --cxl_tiering_enabled=true
            --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads="$PP_THREADS"
            --cxl_pp_threads="$CXL_PP_THREADS"
            --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS"
            --delay_admission_recordcache_threads_start=true
            --dram_buffer_pool_gib="$DRAM_BP_LRU"
         )
         ;;
      dram_ssd)
         admission_mode="lru"
         extra_flags=(
            --cxl_tiering_enabled=false
            --dram_buffer_pool_gib="$DRAM_BP_DRAM_SSD"
            --pp_threads="$PP_THREADS"
         )
         ;;
      dram_ssd_unconstrained)
         admission_mode="lru"
         extra_flags=(
            --cxl_tiering_enabled=false
            --dram_buffer_pool_gib="$DRAM_BP_DRAM_SSD_UNCONSTRAINED"
            --pp_threads="$PP_THREADS"
         )
         ;;
      *)
         echo "[ERROR] unknown variant: $variant"
         return 1
         ;;
   esac

   echo ""
   echo "[RUN][$CURRENT_EXPERIMENT/$TOTAL_EXPERIMENTS] wl=$wl variant=$variant theta=$theta warmup=$warmup_lookups"
   echo "[RUN] result -> $result_file"

   local wl_start_ts wl_end_ts wl_elapsed_sec wl_elapsed_min
   wl_start_ts=$(date +%s)
   local exit_code=0

   "$binary" \
      --test_admission_mode="$admission_mode" \
      --test_zipf_theta="$theta" \
      --test_warmup_lookups="$warmup_lookups" \
      "${extra_flags[@]}" \
      "${COMMON_FLAGS[@]}" \
      2>&1 | tee "$result_file" || exit_code=$?

   wl_end_ts=$(date +%s)
   wl_elapsed_sec=$((wl_end_ts - wl_start_ts))
   wl_elapsed_min=$(awk "BEGIN {printf \"%.2f\", $wl_elapsed_sec / 60.0}")

   if [[ "$exit_code" -ne 0 ]]; then
      echo "[WARN] wl=$wl variant=$variant theta=$theta exit code $exit_code (continuing)"
   fi

   echo "[TIME] wl=$wl variant=$variant theta=$theta elapsed: ${wl_elapsed_sec}s (${wl_elapsed_min} min)" \
      | tee -a "$result_file"

   echo "[INFO] cooldown ${COOLDOWN_SECONDS}s"
   sleep "$COOLDOWN_SECONDS"
}

# -----------------------------------------------------------------------------
# Main — wrapped so we can tee everything to master.log
# -----------------------------------------------------------------------------
main() {
   log_phase "ablation start: ws=${WORKING_SET_GIB} GiB, CXL=${CXL_GIB} GiB"
   echo "[INFO] timestamp                = ${TIMESTAMP}"
   echo "[INFO] result_dir               = ${RESULT_DIR}"
   echo "[INFO] worker_threads           = ${WORKER_THREADS}"
   echo "[INFO] thetas (B/C/A/D/F)       = ${THETAS[*]}"
   echo "[INFO] single theta (E)         = ${SINGLE_THETA}"
   echo "[INFO] variants                 = ${VARIANTS[*]}"
   echo "[INFO] WARMUP_LOOKUPS_TIER1     = ${WARMUP_LOOKUPS_TIER1}  (A/B/C/F)"
   echo "[INFO] WARMUP_LOOKUPS_TIER2     = ${WARMUP_LOOKUPS_TIER2}  (D/E)"
   echo "[INFO] MEASURE_LOOKUPS          = ${MEASURE_LOOKUPS}"
   echo "[INFO] DRAM_BP_DRAM_SSD_UNCONSTRAINED = ${DRAM_BP_DRAM_SSD_UNCONSTRAINED} GiB"
   echo "[INFO] total_experiments        = ${TOTAL_EXPERIMENTS}"

   preflight

   local OVERALL_START
   OVERALL_START=$(date +%s)

   for theta in "${THETAS[@]}"; do
      log_phase "THETA=${theta}  ws=${WORKING_SET_GIB} GiB  (6 wl × ${#VARIANTS[@]} variants)"
      for wl in "${WORKLOADS_FULL[@]}"; do
         for variant in "${VARIANTS[@]}"; do
            run_one "$wl" "$theta" "$variant"
         done
      done
      if [[ "$theta" == "$SINGLE_THETA" ]]; then
         for wl in "${WORKLOAD_SINGLE_THETA[@]}"; do
            log_phase "  WL=${wl} (single theta=${SINGLE_THETA})"
            for variant in "${VARIANTS[@]}"; do
               run_one "$wl" "$SINGLE_THETA" "$variant"
            done
         done
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
