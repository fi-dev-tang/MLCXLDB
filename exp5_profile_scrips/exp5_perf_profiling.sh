#!/usr/bin/env bash

set -euo pipefail
trap '' HUP

# =============================================================================
# Exp5: Perf Profiling (独占 dax0.8 + exp5_perf_profiling SSD)
#
# 按实验七全量规模进行 perf 采集:
#
# Phase 1 — perf stat: 5 modes × 6 YCSB × 3 thetas + 5 TPC-C = 95 groups
# Phase 2 — perf record: 5 modes × 6 YCSB × theta=0.90       = 30 groups
#
# Total: 125 groups
#
# Config same as exp7: WS=23G, CXL=18G, DRAM=3G
# =============================================================================

# -----------------------------------------------------------------------------
# Repo paths (same as exp7)
# -----------------------------------------------------------------------------
BUILD_DIR_WT="/home/zhizhi.tyf/cxl-WT-comparison/build/frontend"
BUILD_DIR_RO="/home/zhizhi.tyf/cxl-recordcache-comparison/build/frontend"

SSD_PATH="/data1/zhizhi.tyf/cxl_test_tmp/exp5_perf_profiling"
CXL_DAX_DEVICE="/dev/dax0.8"

# -----------------------------------------------------------------------------
# Paths
# -----------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="$SCRIPT_DIR/run_${TIMESTAMP}_exp5_perf"
mkdir -p "$RESULT_DIR"
MASTER_LOG="$RESULT_DIR/master.log"

# perf record output subdirectory
FLAMEGRAPH_DIR="$RESULT_DIR/flamegraph_data"
mkdir -p "$FLAMEGRAPH_DIR"

# -----------------------------------------------------------------------------
# Matrix
# -----------------------------------------------------------------------------
MODES=(two_level_readonly two_level_wt bf-tree tiered-indexing-zxj hybried-tier-asplos2025)

# Phase 1: perf stat — all YCSB workloads × all thetas (full exp7 scale)
STAT_WORKLOADS=(b c a d f e)
STAT_THETAS=(0.90 0.95 0.99)

# Phase 2: perf record — all workloads, theta=0.90 (representative)
RECORD_WORKLOADS=(b c a d f e)
RECORD_THETA=0.90

# -----------------------------------------------------------------------------
# perf events (only use events confirmed available on this machine)
# -----------------------------------------------------------------------------
PERF_STAT_EVENTS="cycles,instructions,cache-references,cache-misses,branches,branch-misses,context-switches,cpu-migrations,page-faults,bus-cycles"

PERF_RECORD_FREQ=99

# -----------------------------------------------------------------------------
# Memory config (identical to exp7)
# -----------------------------------------------------------------------------
WS_GIB=23.0
CXL_GIB=18.0
DRAM_TOTAL=3.0

TWO_LEVEL_BP_ABCF=0.43
TWO_LEVEL_RC_ABCF=2.57
TWO_LEVEL_BP_DE=2.57
TWO_LEVEL_RC_DE=0.43
BASELINE_BP=3.0

# -----------------------------------------------------------------------------
# Lookup config
# Phase 1 (perf stat): same warmup as exp7, measure=200M
# Phase 2 (perf record): same warmup, shorter measure=50M to keep perf.data small
# -----------------------------------------------------------------------------
WARMUP_LOOKUPS_TIER1=800000000
WARMUP_LOOKUPS_TIER2=50000000
STAT_MEASURE_LOOKUPS=200000000
RECORD_MEASURE_LOOKUPS=50000000

# TPC-C (perf stat only, same as exp7)
TPCC_WAREHOUSE_COUNT=100
TPCC_WARMUP_SECONDS=120
TPCC_MEASURE_SECONDS=180

# -----------------------------------------------------------------------------
# Threading & misc (same as exp7)
# -----------------------------------------------------------------------------
WORKER_THREADS=20
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
# Helpers (copied from exp7)
# -----------------------------------------------------------------------------
log_phase() {
   echo ""
   echo "============================================================"
   echo "  $1"
   echo "  $(date '+%Y-%m-%d %H:%M:%S')"
   echo "============================================================"
}

log_info() { echo "[INFO] $(date '+%H:%M:%S') $1"; }

is_tier2() {
   case "$1" in
      d|e) return 0 ;;
      *)   return 1 ;;
   esac
}

get_build_dir() {
   case "$1" in
      two_level_readonly) echo "$BUILD_DIR_RO" ;;
      *)                  echo "$BUILD_DIR_WT" ;;
   esac
}

get_admission_mode() {
   case "$1" in
      two_level_readonly|two_level_wt) echo "two_level" ;;
      *) echo "$1" ;;
   esac
}

# Counters
TOTAL_EXPERIMENTS=0
CURRENT_EXPERIMENT=0
PASS_COUNT=0
FAIL_COUNT=0
declare -a FAILED_TESTS=()

# -----------------------------------------------------------------------------
# Build mode-specific flags (identical to exp7)
# -----------------------------------------------------------------------------
build_mode_flags() {
   local mode="$1" wl="$2"
   local admission_mode
   admission_mode="$(get_admission_mode "$mode")"

   local bp_gib rc_gib

   case "$mode" in
      two_level_readonly|two_level_wt)
         if is_tier2 "$wl"; then
            bp_gib="$TWO_LEVEL_BP_DE"
            rc_gib="$TWO_LEVEL_RC_DE"
         else
            bp_gib="$TWO_LEVEL_BP_ABCF"
            rc_gib="$TWO_LEVEL_RC_ABCF"
         fi
         MODE_FLAGS=(
            --test_admission_mode="$admission_mode"
            --cxl_tiering_enabled=true
            --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads="$PP_THREADS"
            --cxl_pp_threads="$CXL_PP_THREADS"
            --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS"
            --delay_admission_recordcache_threads_start=true
            --dram_buffer_pool_gib="$bp_gib"
            --dram_recordcache_gib="$rc_gib"
            --forward_epoch_thread="$FORWARD_EPOCH_THREAD"
            --sieve_eviction_thread="$SIEVE_EVICTION_THREAD"
            --record_cache_promote_thread="$RECORD_CACHE_PROMOTE_THREAD"
         )
         ;;
      bf-tree)
         MODE_FLAGS=(
            --test_admission_mode=bf-tree
            --cxl_tiering_enabled=true
            --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads="$PP_THREADS"
            --cxl_pp_threads="$CXL_PP_THREADS"
            --dram_buffer_pool_gib="$BASELINE_BP"
            --dram_recordcache_gib=0.0
         )
         ;;
      tiered-indexing-zxj)
         MODE_FLAGS=(
            --test_admission_mode=tiered-indexing-zxj
            --cxl_tiering_enabled=true
            --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads="$PP_THREADS"
            --cxl_pp_threads="$CXL_PP_THREADS"
            --dram_buffer_pool_gib="$BASELINE_BP"
            --dram_recordcache_gib=0.0
            --vi_fremove=true
         )
         ;;
      hybried-tier-asplos2025)
         MODE_FLAGS=(
            --test_admission_mode=hybried-tier-asplos2025
            --cxl_tiering_enabled=true
            --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads="$PP_THREADS"
            --cxl_pp_threads="$CXL_PP_THREADS"
            --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS"
            --delay_admission_recordcache_threads_start=true
            --dram_buffer_pool_gib="$BASELINE_BP"
            --dram_recordcache_gib=0.0
         )
         ;;
   esac
}

# =============================================================================
# Phase 1: perf stat — hardware counters
# =============================================================================

run_perf_stat_ycsb() {
   local wl="$1" mode="$2" theta="$3"
   CURRENT_EXPERIMENT=$((CURRENT_EXPERIMENT + 1))

   local build_dir
   build_dir="$(get_build_dir "$mode")"
   local binary="$build_dir/experiment_1_ycsb_${wl}"
   local result_file="$RESULT_DIR/perf_stat_ycsb_${wl}_${mode}_theta${theta}.log"
   local stat_file="$RESULT_DIR/perf_stat_ycsb_${wl}_${mode}_theta${theta}.perf_stat"

   if [[ ! -x "$binary" ]]; then
      echo "[ERROR] binary not found: $binary"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("stat_ycsb_${wl}_${mode}:MISSING_BINARY")
      return 0
   fi

   local warmup="$WARMUP_LOOKUPS_TIER1"
   if is_tier2 "$wl"; then
      warmup="$WARMUP_LOOKUPS_TIER2"
   fi

   build_mode_flags "$mode" "$wl"

   rm -f "$SSD_PATH"

   echo ""
   log_info "[$CURRENT_EXPERIMENT/$TOTAL_EXPERIMENTS] PERF_STAT ycsb_${wl} mode=${mode} theta=${theta}"

   local start_ts exit_code=0
   start_ts=$(date +%s)

   perf stat -e "$PERF_STAT_EVENTS" -o "$stat_file" -- \
      "$binary" \
         --test_zipf_theta="$theta" \
         --test_working_set_gib="$WS_GIB" \
         --test_payload_size_bytes="$PAYLOAD_SIZE_BYTES" \
         --test_warmup_lookups="$warmup" \
         --test_measure_lookups="$STAT_MEASURE_LOOKUPS" \
         --test_progress_interval="$PROGRESS_INTERVAL" \
         --test_warmup_progress_interval="$WARMUP_PROGRESS_INTERVAL" \
         --worker_threads="$WORKER_THREADS" \
         --vi=true --wal=true --trunc=true \
         --ssd_path="$SSD_PATH" \
         "${MODE_FLAGS[@]}" \
         2>&1 | tee "$result_file" || exit_code=$?

   local elapsed=$(( $(date +%s) - start_ts ))

   if [[ "$exit_code" -ne 0 ]]; then
      echo "[FAIL] stat ycsb_${wl} mode=${mode} exit=$exit_code (${elapsed}s)"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("stat_ycsb_${wl}_${mode}:exit${exit_code}")
   else
      echo "[PASS] stat ycsb_${wl} mode=${mode} (${elapsed}s)"
      PASS_COUNT=$((PASS_COUNT + 1))
   fi

   echo "[TIME] stat ycsb_${wl} ${mode} elapsed=${elapsed}s" | tee -a "$result_file"
   sleep "$COOLDOWN_SECONDS"
}

run_perf_stat_tpcc() {
   local mode="$1"
   CURRENT_EXPERIMENT=$((CURRENT_EXPERIMENT + 1))

   local build_dir
   build_dir="$(get_build_dir "$mode")"
   local binary="$build_dir/tpcc_compare_test"
   local result_file="$RESULT_DIR/perf_stat_tpcc_${mode}.log"
   local stat_file="$RESULT_DIR/perf_stat_tpcc_${mode}.perf_stat"

   if [[ ! -x "$binary" ]]; then
      echo "[ERROR] binary not found: $binary"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("stat_tpcc_${mode}:MISSING_BINARY")
      return 0
   fi

   build_mode_flags "$mode" "a"

   rm -f "$SSD_PATH"

   echo ""
   log_info "[$CURRENT_EXPERIMENT/$TOTAL_EXPERIMENTS] PERF_STAT tpcc mode=${mode}"

   local start_ts exit_code=0
   start_ts=$(date +%s)

   perf stat -e "$PERF_STAT_EVENTS" -o "$stat_file" -- \
      "$binary" \
         --tpcc_warehouse_count="$TPCC_WAREHOUSE_COUNT" \
         --test_warmup_seconds="$TPCC_WARMUP_SECONDS" \
         --test_measure_seconds="$TPCC_MEASURE_SECONDS" \
         --test_load_data=true \
         --worker_threads="$WORKER_THREADS" \
         --vi=true --wal=true --trunc=true \
         --ssd_path="$SSD_PATH" \
         "${MODE_FLAGS[@]}" \
         2>&1 | tee "$result_file" || exit_code=$?

   local elapsed=$(( $(date +%s) - start_ts ))

   if [[ "$exit_code" -ne 0 ]]; then
      echo "[FAIL] stat tpcc mode=${mode} exit=$exit_code (${elapsed}s)"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("stat_tpcc_${mode}:exit${exit_code}")
   else
      echo "[PASS] stat tpcc mode=${mode} (${elapsed}s)"
      PASS_COUNT=$((PASS_COUNT + 1))
   fi

   echo "[TIME] stat tpcc ${mode} elapsed=${elapsed}s" | tee -a "$result_file"
   sleep "$COOLDOWN_SECONDS"
}

# =============================================================================
# Phase 2: perf record — flame-graph data
# =============================================================================

run_perf_record_ycsb() {
   local wl="$1" mode="$2" theta="$3"
   CURRENT_EXPERIMENT=$((CURRENT_EXPERIMENT + 1))

   local build_dir
   build_dir="$(get_build_dir "$mode")"
   local binary="$build_dir/experiment_1_ycsb_${wl}"
   local result_file="$RESULT_DIR/perf_record_ycsb_${wl}_${mode}_theta${theta}.log"
   local perf_data="$FLAMEGRAPH_DIR/perf_ycsb_${wl}_${mode}_theta${theta}.data"

   if [[ ! -x "$binary" ]]; then
      echo "[ERROR] binary not found: $binary"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("record_ycsb_${wl}_${mode}:MISSING_BINARY")
      return 0
   fi

   local warmup="$WARMUP_LOOKUPS_TIER1"
   if is_tier2 "$wl"; then
      warmup="$WARMUP_LOOKUPS_TIER2"
   fi

   build_mode_flags "$mode" "$wl"

   rm -f "$SSD_PATH"

   echo ""
   log_info "[$CURRENT_EXPERIMENT/$TOTAL_EXPERIMENTS] PERF_RECORD ycsb_${wl} mode=${mode} theta=${theta}"

   local start_ts exit_code=0
   start_ts=$(date +%s)

   perf record -g -F "$PERF_RECORD_FREQ" -o "$perf_data" -- \
      "$binary" \
         --test_zipf_theta="$theta" \
         --test_working_set_gib="$WS_GIB" \
         --test_payload_size_bytes="$PAYLOAD_SIZE_BYTES" \
         --test_warmup_lookups="$warmup" \
         --test_measure_lookups="$RECORD_MEASURE_LOOKUPS" \
         --test_progress_interval="$PROGRESS_INTERVAL" \
         --test_warmup_progress_interval="$WARMUP_PROGRESS_INTERVAL" \
         --worker_threads="$WORKER_THREADS" \
         --vi=true --wal=true --trunc=true \
         --ssd_path="$SSD_PATH" \
         "${MODE_FLAGS[@]}" \
         2>&1 | tee "$result_file" || exit_code=$?

   local elapsed=$(( $(date +%s) - start_ts ))
   local data_size
   data_size="$(du -sh "$perf_data" 2>/dev/null | cut -f1 || echo "N/A")"

   if [[ "$exit_code" -ne 0 ]]; then
      echo "[FAIL] record ycsb_${wl} mode=${mode} exit=$exit_code (${elapsed}s)"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("record_ycsb_${wl}_${mode}:exit${exit_code}")
   else
      echo "[PASS] record ycsb_${wl} mode=${mode} (${elapsed}s, perf.data=${data_size})"
      PASS_COUNT=$((PASS_COUNT + 1))
   fi

   echo "[TIME] record ycsb_${wl} ${mode} elapsed=${elapsed}s perf.data=${data_size}" | tee -a "$result_file"
   sleep "$COOLDOWN_SECONDS"
}

# -----------------------------------------------------------------------------
# Pre-flight checks
# -----------------------------------------------------------------------------
preflight() {
   if [[ ! -e "$CXL_DAX_DEVICE" ]]; then
      echo "[ERROR] dax device $CXL_DAX_DEVICE not found"; return 1
   fi
   if ! command -v perf >/dev/null 2>&1; then
      echo "[ERROR] perf not found in PATH"; return 1
   fi
   for build_dir in "$BUILD_DIR_WT" "$BUILD_DIR_RO"; do
      for wl in "${STAT_WORKLOADS[@]}"; do
         if [[ ! -x "$build_dir/experiment_1_ycsb_${wl}" ]]; then
            echo "[WARN] binary missing: $build_dir/experiment_1_ycsb_${wl}"
         fi
      done
      if [[ ! -x "$build_dir/tpcc_compare_test" ]]; then
         echo "[WARN] tpcc binary missing: $build_dir/tpcc_compare_test"
      fi
   done
}

# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------
main() {
   # Phase 1: 5 modes × 6 YCSB × 3 thetas + 5 TPC-C = 95
   # Phase 2: 5 modes × 6 YCSB × 1 theta             = 30
   TOTAL_EXPERIMENTS=$(( ${#MODES[@]} * ${#STAT_WORKLOADS[@]} * ${#STAT_THETAS[@]} + ${#MODES[@]} + ${#MODES[@]} * ${#RECORD_WORKLOADS[@]} ))

   log_phase "Exp5 Perf Profiling (WS=${WS_GIB}G, CXL=${CXL_GIB}G, DRAM=${DRAM_TOTAL}G)"
   log_info "timestamp        = ${TIMESTAMP}"
   log_info "result_dir       = ${RESULT_DIR}"
   log_info "flamegraph_dir   = ${FLAMEGRAPH_DIR}"
   log_info "build_dir (WT)   = ${BUILD_DIR_WT}"
   log_info "build_dir (RO)   = ${BUILD_DIR_RO}"
   log_info "ssd_path         = ${SSD_PATH}"
   log_info "cxl_device       = ${CXL_DAX_DEVICE}"
   log_info "modes            = ${MODES[*]}"
   log_info "stat_workloads   = ${STAT_WORKLOADS[*]}"
   log_info "stat_thetas      = ${STAT_THETAS[*]}"
   log_info "record_workloads = ${RECORD_WORKLOADS[*]}"
   log_info "record_theta     = ${RECORD_THETA}"
   log_info "perf_events      = ${PERF_STAT_EVENTS}"
   log_info "perf_record_freq = ${PERF_RECORD_FREQ} Hz"
   log_info "total_experiments= ${TOTAL_EXPERIMENTS}"

   preflight

   local OVERALL_START
   OVERALL_START=$(date +%s)

   # =================================================================
   # PHASE 1: perf stat — full exp7 scale, all thetas
   # =================================================================
   log_phase "PHASE 1: perf stat (${#MODES[@]} modes × ${#STAT_WORKLOADS[@]} YCSB × ${#STAT_THETAS[@]} thetas + ${#MODES[@]} TPC-C = 95 groups)"

   for theta in "${STAT_THETAS[@]}"; do
      for wl in "${STAT_WORKLOADS[@]}"; do
         log_phase "PERF_STAT YCSB-${wl^^} theta=${theta}"
         for mode in "${MODES[@]}"; do
            run_perf_stat_ycsb "$wl" "$mode" "$theta"
         done
      done
   done

   log_phase "PERF_STAT TPC-C"
   for mode in "${MODES[@]}"; do
      run_perf_stat_tpcc "$mode"
   done

   # =================================================================
   # PHASE 2: perf record — flame-graph data, all workloads, theta=0.90
   # =================================================================
   log_phase "PHASE 2: perf record (${#MODES[@]} modes × ${#RECORD_WORKLOADS[@]} WLs × theta=${RECORD_THETA}, measure=${RECORD_MEASURE_LOOKUPS} = 30 groups)"

   for wl in "${RECORD_WORKLOADS[@]}"; do
      log_phase "PERF_RECORD YCSB-${wl^^} theta=${RECORD_THETA}"
      for mode in "${MODES[@]}"; do
         run_perf_record_ycsb "$wl" "$mode" "$RECORD_THETA"
      done
   done

   # =================================================================
   # Summary
   # =================================================================
   local OVERALL_SEC OVERALL_HR
   OVERALL_SEC=$(( $(date +%s) - OVERALL_START ))
   OVERALL_HR=$(awk "BEGIN {printf \"%.2f\", $OVERALL_SEC / 3600.0}")

   log_phase "EXP5 PERF PROFILING COMPLETE"
   log_info "total elapsed = ${OVERALL_SEC}s (${OVERALL_HR} hr)"
   log_info "pass=${PASS_COUNT} fail=${FAIL_COUNT} / ${TOTAL_EXPERIMENTS}"

   if [[ ${#FAILED_TESTS[@]} -gt 0 ]]; then
      echo ""
      echo "FAILED TESTS:"
      for t in "${FAILED_TESTS[@]}"; do
         echo "  - $t"
      done
   fi

   echo ""
   echo "Results in: $RESULT_DIR"
   echo ""
   echo "To generate flame graphs from perf.data files:"
   echo "  cd $FLAMEGRAPH_DIR"
   echo "  perf script -i <perf_data_file> | stackcollapse-perf.pl | flamegraph.pl > out.svg"
   echo ""
   echo "perf stat summaries are in *.perf_stat files"

   # --- Generate a summary CSV for easy comparison ---
   local csv="$RESULT_DIR/perf_stat_summary.csv"
   echo "test,mode,cycles,instructions,IPC,cache_refs,cache_misses,cache_miss_pct,branches,branch_misses,branch_miss_pct,context_switches,page_faults" > "$csv"

   for stat_file in "$RESULT_DIR"/*.perf_stat; do
      [[ -f "$stat_file" ]] || continue
      local base
      base="$(basename "$stat_file" .perf_stat)"
      local test_name="${base#perf_stat_}"
      local mode_name=""
      for m in "${MODES[@]}"; do
         if [[ "$base" == *"_${m}"* ]]; then
            mode_name="$m"
            break
         fi
      done

      local cyc ins cref cmiss br brmiss cs pf
      cyc="$(grep -oP '[\d,]+(?=\s+cycles)' "$stat_file" 2>/dev/null | tr -d ',' || echo "0")"
      ins="$(grep -oP '[\d,]+(?=\s+instructions)' "$stat_file" 2>/dev/null | tr -d ',' || echo "0")"
      cref="$(grep -oP '[\d,]+(?=\s+cache-references)' "$stat_file" 2>/dev/null | tr -d ',' || echo "0")"
      cmiss="$(grep -oP '[\d,]+(?=\s+cache-misses)' "$stat_file" 2>/dev/null | tr -d ',' || echo "0")"
      br="$(grep -oP '[\d,]+(?=\s+branches)' "$stat_file" 2>/dev/null | tr -d ',' || echo "0")"
      brmiss="$(grep -oP '[\d,]+(?=\s+branch-misses)' "$stat_file" 2>/dev/null | tr -d ',' || echo "0")"
      cs="$(grep -oP '[\d,]+(?=\s+context-switches)' "$stat_file" 2>/dev/null | tr -d ',' || echo "0")"
      pf="$(grep -oP '[\d,]+(?=\s+page-faults)' "$stat_file" 2>/dev/null | tr -d ',' || echo "0")"

      local ipc cmiss_pct brmiss_pct
      ipc="$(awk "BEGIN {if ($cyc>0) printf \"%.2f\", $ins/$cyc; else print \"0\"}")"
      cmiss_pct="$(awk "BEGIN {if ($cref>0) printf \"%.2f\", $cmiss*100.0/$cref; else print \"0\"}")"
      brmiss_pct="$(awk "BEGIN {if ($br>0) printf \"%.2f\", $brmiss*100.0/$br; else print \"0\"}")"

      echo "${test_name},${mode_name},${cyc},${ins},${ipc},${cref},${cmiss},${cmiss_pct},${br},${brmiss},${brmiss_pct},${cs},${pf}" >> "$csv"
   done

   log_info "Summary CSV: $csv"
}

main 2>&1 | tee "$MASTER_LOG"
