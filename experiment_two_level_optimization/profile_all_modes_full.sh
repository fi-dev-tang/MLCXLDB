#!/usr/bin/env bash
#
# Full-coverage perf profiling: 4 modes × 7 workloads = 28 runs.
#
# Configuration mirrors run_medium_benchmark.sh exactly so the profile
# reflects the same setup as the baseline comparison numbers in
# bench_20260526_150208/. Differences from the previous (hot-path-only)
# profile script:
#
#   - 28 runs (was 8): A B C D E F + TPC-C × {two_level, bf-tree,
#                      hybridtier-asplos2025, tiered-indexing-zxj}
#   - Full workload per run (warmup 20M + measure 30M; D/E warmup 10M):
#     perf covers the entire process lifecycle including warmup, so we
#     can see whether warmup-time admission CMS work or steady-state
#     hit path dominates.
#   - perf.data lives on /data1 (big disk) — 28 × 10-20 GB ≈ 300-500 GB
#   - 1h timeout per run (was implicit short timeout)
#   - D/E use the rebalanced DRAM split (BP=0.375 + RC=0.125) the same
#     way medium_benchmark does — RC_HR is ~0 in those workloads so the
#     RC budget would be dead weight; this is the configuration we
#     actually want to profile.
#
# Two-pass per run:
#   pass 1: perf stat  — high-level metrics (IPC, cache miss, branches)
#   pass 2: perf record — stack-attributed samples for flamegraphs
#
# Layout:
#   /data1/zhizhi.tyf/cxl_test_tmp/profile_<TS>/<tag>.perf.data    (raw)
#   experiment_two_level_optimization/profile_full_<TS>/<tag>.{log,stat.txt,report.txt,folded,svg,cache.svg}
#

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/frontend"

# ----- isolation: dax + ssd paths must not collide with other experiments -----
CXL_DAX_DEVICE="${CXL_DAX_DEVICE:-/dev/dax0.3}"
SSD_PATH="${SSD_PATH:-/data1/zhizhi.tyf/cxl_test_tmp/profile_workload_tmp}"
mkdir -p "$(dirname "$SSD_PATH")"

# perf.data goes on big disk
PERF_DATA_BASE="${PERF_DATA_BASE:-/data1/zhizhi.tyf/cxl_test_tmp}"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
PERF_DATA_DIR="$PERF_DATA_BASE/profile_${TIMESTAMP}"
mkdir -p "$PERF_DATA_DIR"

# processed outputs (svg, report, csv) stay in the project
RESULT_DIR="$SCRIPT_DIR/profile_full_${TIMESTAMP}"
mkdir -p "$RESULT_DIR"

# pointer file so the result dir knows where its raw data lives
echo "$PERF_DATA_DIR" > "$RESULT_DIR/.perf_data_dir"

FLAMEGRAPH_DIR="${FLAMEGRAPH_DIR:-/home/zhizhi.tyf/FlameGraph}"
if [[ ! -x "$FLAMEGRAPH_DIR/flamegraph.pl" ]]; then
   echo "[ERROR] flamegraph.pl not found at $FLAMEGRAPH_DIR" >&2
   exit 1
fi

# ===========================================================================
# Workload config — VERBATIM from run_medium_benchmark.sh (do not change
# without also changing medium_benchmark.sh, otherwise the profile reflects
# a different setup than the baseline comparison).
# ===========================================================================
WORKING_SET_GIB=2.0
PAYLOAD_SIZE_BYTES=100
CXL_GIB=2.0
ZIPF_THETA=0.99

DRAM_BP_TWO_LEVEL=0.125
DRAM_RC_TWO_LEVEL=0.375
DRAM_BP_BASELINE=0.5

WORKER_THREADS=8
PP_THREADS=1
CXL_PP_THREADS=1
TWO_LEVEL_ADMISSION_THREADS=2
FORWARD_EPOCH_THREAD=1
SIEVE_EVICTION_THREAD=1
RECORD_CACHE_PROMOTE_THREAD=4

WARMUP_LOOKUPS=20000000
MEASURE_LOOKUPS=30000000
PROGRESS_INTERVAL=5000000

TPCC_WAREHOUSE_COUNT=10
TPCC_WARMUP_SECONDS=20
TPCC_MEASURE_SECONDS=40

# ===========================================================================
# perf config
# ===========================================================================
PERF_FREQ=997                       # prime, avoids tick aliasing
PERF_CALLGRAPH_STACK=8192           # dwarf user-stack dump bytes
PERF_TIMEOUT=3600                   # 1h per run (user-requested)
COOLDOWN_SECONDS=10

MODES=(two_level bf-tree hybried-tier-asplos2025 tiered-indexing-zxj)
YCSB_WORKLOADS=(a b c d e f)
TOTAL=$(( ${#MODES[@]} * (${#YCSB_WORKLOADS[@]} + 1) ))
CURRENT=0
PASS_COUNT=0
FAIL_COUNT=0
declare -a FAILED_TESTS=()

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*"; }

# -------------------------------------------------------------------
# mode_flags <mode> <bp_gib> <rc_gib>
# Build the mode-specific flag list. bp/rc only consulted for two_level.
# -------------------------------------------------------------------
mode_flags() {
   local mode="$1"
   local bp="$2"
   local rc="$3"
   case "$mode" in
      two_level)
         echo --test_admission_mode=two_level \
              --cxl_tiering_enabled=true --cxl_gib="$CXL_GIB" \
              --cxl_dax_device_path="$CXL_DAX_DEVICE" \
              --pp_threads="$PP_THREADS" --cxl_pp_threads="$CXL_PP_THREADS" \
              --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS" \
              --delay_admission_recordcache_threads_start=true \
              --dram_buffer_pool_gib="$bp" \
              --dram_recordcache_gib="$rc" \
              --forward_epoch_thread="$FORWARD_EPOCH_THREAD" \
              --sieve_eviction_thread="$SIEVE_EVICTION_THREAD" \
              --record_cache_promote_thread="$RECORD_CACHE_PROMOTE_THREAD"
         ;;
      bf-tree)
         echo --test_admission_mode=bf-tree \
              --cxl_tiering_enabled=true --cxl_gib="$CXL_GIB" \
              --cxl_dax_device_path="$CXL_DAX_DEVICE" \
              --pp_threads="$PP_THREADS" --cxl_pp_threads="$CXL_PP_THREADS" \
              --dram_buffer_pool_gib="$DRAM_BP_BASELINE" \
              --dram_recordcache_gib=0.0
         ;;
      tiered-indexing-zxj)
         echo --test_admission_mode=tiered-indexing-zxj \
              --cxl_tiering_enabled=true --cxl_gib="$CXL_GIB" \
              --cxl_dax_device_path="$CXL_DAX_DEVICE" \
              --pp_threads="$PP_THREADS" --cxl_pp_threads="$CXL_PP_THREADS" \
              --dram_buffer_pool_gib="$DRAM_BP_BASELINE" \
              --dram_recordcache_gib=0.0 \
              --vi_fremove=true
         ;;
      hybried-tier-asplos2025)
         echo --test_admission_mode=hybried-tier-asplos2025 \
              --cxl_tiering_enabled=true --cxl_gib="$CXL_GIB" \
              --cxl_dax_device_path="$CXL_DAX_DEVICE" \
              --pp_threads="$PP_THREADS" --cxl_pp_threads="$CXL_PP_THREADS" \
              --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS" \
              --delay_admission_recordcache_threads_start=true \
              --dram_buffer_pool_gib="$DRAM_BP_BASELINE" \
              --dram_recordcache_gib=0.0
         ;;
   esac
}

# -------------------------------------------------------------------
# Patterns to keep when filtering the folded stacks to the cache-layer
# subset — different per mode because each baseline names its cache
# layer differently.
# -------------------------------------------------------------------
cache_subset_pattern() {
   case "$1" in
      two_level)
         echo "RecordCache|tryLookupInRecordCache|tryWriteThroughSync|tryInvalidateForRemove|GetFromRecordCache|HashBytes|SeqLock|SieveFIFO|TwoLevelAdmission|RecordCMS|PageCMS|EpochManager|PromoteThread|SlabAllocator"
         ;;
      bf-tree)
         echo "BfTree|MiniPage|mini_page|mini_hit|HotPageCandidates|BufferFrame"
         ;;
      hybried-tier-asplos2025)
         echo "HybridTier|CountBloom|MomentumSketch|FourGrid|HotPageCandidates|hot_page_candidates|frequency_cbf|momentum_sketch"
         ;;
      tiered-indexing-zxj)
         echo "TieredIndexing|tiered_indexing|OptimisticLockTable|HotPageCandidates"
         ;;
   esac
}

# -------------------------------------------------------------------
# run_ycsb <workload> <mode>
# -------------------------------------------------------------------
run_ycsb() {
   local wl="$1"
   local mode="$2"

   CURRENT=$((CURRENT + 1))

   local binary="$BUILD_DIR/experiment_1_ycsb_${wl}"
   local tag="ycsb_${wl}_${mode}"
   local stdout_log="$RESULT_DIR/${tag}.log"
   local stat="$RESULT_DIR/${tag}.stat.txt"
   local perf_data="$PERF_DATA_DIR/${tag}.perf.data"
   local report="$RESULT_DIR/${tag}.report.txt"
   local folded="$RESULT_DIR/${tag}.folded"
   local svg="$RESULT_DIR/${tag}.svg"

   if [[ ! -x "$binary" ]]; then
      echo "[ERROR] $binary missing"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("${tag}:MISSING_BINARY")
      return 1
   fi

   # Match medium_benchmark.sh: D/E rebalance + reduced warmup
   local warmup="$WARMUP_LOOKUPS"
   local bp_gib="$DRAM_BP_TWO_LEVEL"
   local rc_gib="$DRAM_RC_TWO_LEVEL"
   case "$wl" in
      d|e)
         warmup=10000000
         bp_gib=0.375
         rc_gib=0.125
         ;;
   esac

   local mflags
   mflags=$(mode_flags "$mode" "$bp_gib" "$rc_gib")

   local common_args=(
      --test_zipf_theta="$ZIPF_THETA"
      --test_working_set_gib="$WORKING_SET_GIB"
      --test_payload_size_bytes="$PAYLOAD_SIZE_BYTES"
      --test_warmup_lookups="$warmup"
      --test_measure_lookups="$MEASURE_LOOKUPS"
      --test_progress_interval="$PROGRESS_INTERVAL"
      --worker_threads="$WORKER_THREADS"
      --vi=true --wal=true --trunc=true
      --ssd_path="$SSD_PATH"
   )

   log ""
   log "[$CURRENT/$TOTAL] ===== $tag (warmup=$warmup, BP=$bp_gib+RC=$rc_gib) ====="

   local start_ts exit_code=0
   start_ts=$(date +%s)

   # --- pass 1: perf stat ---
   log "  pass 1/2: perf stat (entire process)"
   rm -f "$SSD_PATH"
   timeout "$PERF_TIMEOUT" perf stat \
      -e task-clock,context-switches,cpu-migrations,cycles,instructions,branches,branch-misses,cache-references,cache-misses,L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses \
      -o "$stat" \
      -- "$binary" "${common_args[@]}" $mflags \
      >"${stdout_log}.stat_pass" 2>&1 || exit_code=$?
   if [[ "$exit_code" -ne 0 ]]; then
      log "  [WARN] perf stat exit=$exit_code (may have timed out at ${PERF_TIMEOUT}s)"
   fi

   sleep "$COOLDOWN_SECONDS"
   exit_code=0

   # --- pass 2: perf record (entire process) ---
   log "  pass 2/2: perf record -F $PERF_FREQ --call-graph dwarf,$PERF_CALLGRAPH_STACK"
   rm -f "$SSD_PATH"
   timeout "$PERF_TIMEOUT" perf record -F "$PERF_FREQ" \
      --call-graph "dwarf,$PERF_CALLGRAPH_STACK" \
      -o "$perf_data" \
      -- "$binary" "${common_args[@]}" $mflags \
      >"$stdout_log" 2>&1 || exit_code=$?
   if [[ "$exit_code" -ne 0 ]]; then
      log "  [WARN] perf record exit=$exit_code"
   fi

   if [[ ! -s "$perf_data" ]]; then
      log "  [ERROR] perf.data empty/missing — $tag failed"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("${tag}:NO_PERF_DATA")
      sleep "$COOLDOWN_SECONDS"
      return 1
   fi

   local perf_size
   perf_size=$(stat -c%s "$perf_data" 2>/dev/null | awk '{printf "%.1f MB", $1/1024/1024}')
   log "  perf.data: $perf_size"

   # --- post-process ---
   log "  post-process: perf report"
   perf report -i "$perf_data" --stdio --no-children --percent-limit 0.3 \
      > "$report" 2>/dev/null || true

   log "  post-process: stackcollapse + flamegraph"
   perf script -i "$perf_data" 2>/dev/null \
      | "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" 2>/dev/null \
      > "$folded"

   if [[ -s "$folded" ]]; then
      "$FLAMEGRAPH_DIR/flamegraph.pl" \
         --title="${mode} YCSB-${wl^^} full process" \
         --subtitle="warmup=${warmup} measure=${MEASURE_LOOKUPS}, BP=$bp_gib+RC=$rc_gib, dax=$CXL_DAX_DEVICE" \
         "$folded" > "$svg" 2>/dev/null || true
   else
      log "  [WARN] folded empty for $tag"
   fi

   local pat
   pat=$(cache_subset_pattern "$mode")
   local folded_sub="$RESULT_DIR/${tag}.cache.folded"
   local svg_sub="$RESULT_DIR/${tag}.cache.svg"
   grep -E "$pat" "$folded" > "$folded_sub" 2>/dev/null || true
   if [[ -s "$folded_sub" ]]; then
      "$FLAMEGRAPH_DIR/flamegraph.pl" \
         --title="${mode} YCSB-${wl^^} — cache-layer subset" \
         --subtitle="filtered to ${mode} cache frames" \
         "$folded_sub" > "$svg_sub" 2>/dev/null || true
   fi

   local elapsed=$(( $(date +%s) - start_ts ))
   log "  done ($tag) in ${elapsed}s"
   PASS_COUNT=$((PASS_COUNT + 1))

   sleep "$COOLDOWN_SECONDS"
}

# -------------------------------------------------------------------
# run_tpcc <mode>
# -------------------------------------------------------------------
run_tpcc() {
   local mode="$1"

   CURRENT=$((CURRENT + 1))

   local binary="$BUILD_DIR/tpcc_compare_test"
   local tag="tpcc_${mode}"
   local stdout_log="$RESULT_DIR/${tag}.log"
   local stat="$RESULT_DIR/${tag}.stat.txt"
   local perf_data="$PERF_DATA_DIR/${tag}.perf.data"
   local report="$RESULT_DIR/${tag}.report.txt"
   local folded="$RESULT_DIR/${tag}.folded"
   local svg="$RESULT_DIR/${tag}.svg"

   if [[ ! -x "$binary" ]]; then
      echo "[ERROR] $binary missing"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("${tag}:MISSING_BINARY")
      return 1
   fi

   local mflags
   mflags=$(mode_flags "$mode" "$DRAM_BP_TWO_LEVEL" "$DRAM_RC_TWO_LEVEL")

   local common_args=(
      --tpcc_warehouse_count="$TPCC_WAREHOUSE_COUNT"
      --test_warmup_seconds="$TPCC_WARMUP_SECONDS"
      --test_measure_seconds="$TPCC_MEASURE_SECONDS"
      --test_load_data=true
      --worker_threads="$WORKER_THREADS"
      --vi=true --wal=true --trunc=true
      --ssd_path="$SSD_PATH"
   )

   log ""
   log "[$CURRENT/$TOTAL] ===== $tag (wh=$TPCC_WAREHOUSE_COUNT measure=${TPCC_MEASURE_SECONDS}s) ====="

   local start_ts exit_code=0
   start_ts=$(date +%s)

   log "  pass 1/2: perf stat"
   rm -f "$SSD_PATH"
   timeout "$PERF_TIMEOUT" perf stat \
      -e task-clock,context-switches,cpu-migrations,cycles,instructions,branches,branch-misses,cache-references,cache-misses,L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses \
      -o "$stat" \
      -- "$binary" "${common_args[@]}" $mflags \
      >"${stdout_log}.stat_pass" 2>&1 || exit_code=$?
   if [[ "$exit_code" -ne 0 ]]; then
      log "  [WARN] perf stat exit=$exit_code"
   fi

   sleep "$COOLDOWN_SECONDS"
   exit_code=0

   log "  pass 2/2: perf record"
   rm -f "$SSD_PATH"
   timeout "$PERF_TIMEOUT" perf record -F "$PERF_FREQ" \
      --call-graph "dwarf,$PERF_CALLGRAPH_STACK" \
      -o "$perf_data" \
      -- "$binary" "${common_args[@]}" $mflags \
      >"$stdout_log" 2>&1 || exit_code=$?
   if [[ "$exit_code" -ne 0 ]]; then
      log "  [WARN] perf record exit=$exit_code"
   fi

   if [[ ! -s "$perf_data" ]]; then
      log "  [ERROR] perf.data empty — $tag failed"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("${tag}:NO_PERF_DATA")
      sleep "$COOLDOWN_SECONDS"
      return 1
   fi

   local perf_size
   perf_size=$(stat -c%s "$perf_data" 2>/dev/null | awk '{printf "%.1f MB", $1/1024/1024}')
   log "  perf.data: $perf_size"

   log "  post-process"
   perf report -i "$perf_data" --stdio --no-children --percent-limit 0.3 \
      > "$report" 2>/dev/null || true
   perf script -i "$perf_data" 2>/dev/null \
      | "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" 2>/dev/null \
      > "$folded"
   if [[ -s "$folded" ]]; then
      "$FLAMEGRAPH_DIR/flamegraph.pl" \
         --title="${mode} TPC-C full process" \
         --subtitle="wh=$TPCC_WAREHOUSE_COUNT measure=${TPCC_MEASURE_SECONDS}s, dax=$CXL_DAX_DEVICE" \
         "$folded" > "$svg" 2>/dev/null || true
   fi

   local pat
   pat=$(cache_subset_pattern "$mode")
   local folded_sub="$RESULT_DIR/${tag}.cache.folded"
   local svg_sub="$RESULT_DIR/${tag}.cache.svg"
   grep -E "$pat" "$folded" > "$folded_sub" 2>/dev/null || true
   if [[ -s "$folded_sub" ]]; then
      "$FLAMEGRAPH_DIR/flamegraph.pl" \
         --title="${mode} TPC-C — cache-layer subset" \
         --subtitle="filtered to ${mode} cache frames" \
         "$folded_sub" > "$svg_sub" 2>/dev/null || true
   fi

   local elapsed=$(( $(date +%s) - start_ts ))
   log "  done ($tag) in ${elapsed}s"
   PASS_COUNT=$((PASS_COUNT + 1))

   sleep "$COOLDOWN_SECONDS"
}

# -------------------------------------------------------------------
# build_summary
# -------------------------------------------------------------------
build_summary() {
   local csv="$RESULT_DIR/summary.csv"
   {
      echo "tag,workload,mode,mqps_or_tps,RC_HR,ipc,cycles_billion,L1d_miss_billion,LLC_miss_billion,perf_data_MB"
      for wl in "${YCSB_WORKLOADS[@]}"; do
         for m in "${MODES[@]}"; do
            local tag="ycsb_${wl}_${m}"
            local log="$RESULT_DIR/${tag}.log"
            local stat="$RESULT_DIR/${tag}.stat.txt"
            local pd="$PERF_DATA_DIR/${tag}.perf.data"
            local mqps="" rc="" ipc="" cyc="" l1miss="" llcmiss="" pdmb=""
            if [[ -f "$log" ]]; then
               local final
               final=$(grep -E "^.*Mqps=|mode=.*Mqps" "$log" | tail -1)
               mqps=$(echo "$final" | grep -oP 'Mqps=\K[0-9.]+' || echo "")
               rc=$(  echo "$final" | grep -oP 'RC_HR=\K[0-9.]+' || echo "")
            fi
            if [[ -f "$stat" ]]; then
               ipc=$(grep -oP '#\s*\K[0-9.]+(?=\s+insn per cycle)' "$stat" | head -1)
               local cyc_raw l1_raw llc_raw
               cyc_raw=$(grep -oP '^\s*\K[0-9,]+(?=\s+cycles)'                "$stat" | head -1 | tr -d ',')
               l1_raw=$( grep -oP '^\s*\K[0-9,]+(?=\s+L1-dcache-load-misses)' "$stat" | head -1 | tr -d ',')
               llc_raw=$(grep -oP '^\s*\K[0-9,]+(?=\s+LLC-load-misses)'       "$stat" | head -1 | tr -d ',')
               [[ -n "$cyc_raw"  ]] && cyc=$(   awk -v c="$cyc_raw"  'BEGIN{printf "%.2f", c/1e9}')
               [[ -n "$l1_raw"   ]] && l1miss=$(awk -v c="$l1_raw"   'BEGIN{printf "%.2f", c/1e9}')
               [[ -n "$llc_raw"  ]] && llcmiss=$(awk -v c="$llc_raw" 'BEGIN{printf "%.2f", c/1e9}')
            fi
            if [[ -f "$pd" ]]; then
               pdmb=$(stat -c%s "$pd" | awk '{printf "%.1f", $1/1024/1024}')
            fi
            echo "${tag},${wl},${m},${mqps},${rc},${ipc},${cyc},${l1miss},${llcmiss},${pdmb}"
         done
      done
      # tpcc rows
      for m in "${MODES[@]}"; do
         local tag="tpcc_${m}"
         local log="$RESULT_DIR/${tag}.log"
         local stat="$RESULT_DIR/${tag}.stat.txt"
         local pd="$PERF_DATA_DIR/${tag}.perf.data"
         local tps="" rc="" ipc="" cyc="" l1miss="" llcmiss="" pdmb=""
         if [[ -f "$log" ]]; then
            tps=$(grep -oP '^TPS:\s+\K[0-9.]+' "$log" | tail -1)
            rc=$(grep -oP 'Record cache hit/miss:.*\(\K[0-9.]+' "$log" | tail -1)
         fi
         if [[ -f "$stat" ]]; then
            ipc=$(grep -oP '#\s*\K[0-9.]+(?=\s+insn per cycle)' "$stat" | head -1)
            local cyc_raw l1_raw llc_raw
            cyc_raw=$(grep -oP '^\s*\K[0-9,]+(?=\s+cycles)'                "$stat" | head -1 | tr -d ',')
            l1_raw=$( grep -oP '^\s*\K[0-9,]+(?=\s+L1-dcache-load-misses)' "$stat" | head -1 | tr -d ',')
            llc_raw=$(grep -oP '^\s*\K[0-9,]+(?=\s+LLC-load-misses)'       "$stat" | head -1 | tr -d ',')
            [[ -n "$cyc_raw"  ]] && cyc=$(   awk -v c="$cyc_raw"  'BEGIN{printf "%.2f", c/1e9}')
            [[ -n "$l1_raw"   ]] && l1miss=$(awk -v c="$l1_raw"   'BEGIN{printf "%.2f", c/1e9}')
            [[ -n "$llc_raw"  ]] && llcmiss=$(awk -v c="$llc_raw" 'BEGIN{printf "%.2f", c/1e9}')
         fi
         if [[ -f "$pd" ]]; then
            pdmb=$(stat -c%s "$pd" | awk '{printf "%.1f", $1/1024/1024}')
         fi
         echo "${tag},tpcc,${m},${tps},${rc},${ipc},${cyc},${l1miss},${llcmiss},${pdmb}"
      done
   } > "$csv"

   echo ""
   log "summary csv: $csv"
   column -s, -t < "$csv"
}

# -------------------------------------------------------------------
# main
# -------------------------------------------------------------------
log "result dir (processed) : $RESULT_DIR"
log "perf.data dir (raw)    : $PERF_DATA_DIR"
log "cxl_dax                : $CXL_DAX_DEVICE"
log "ssd_path               : $SSD_PATH"
log "perf                   : $(perf --version 2>&1)"
log "modes                  : ${MODES[*]}"
log "workloads              : YCSB ${YCSB_WORKLOADS[*]} + TPC-C"
log "total runs             : $TOTAL  (each = stat pass + record pass)"
log "perf_record freq       : $PERF_FREQ Hz, --call-graph dwarf,$PERF_CALLGRAPH_STACK"
log "timeout                : ${PERF_TIMEOUT}s per run"
log "/data1 free            : $(df -h /data1 | awk 'NR==2 {print $4}')"

# Sanity: nothing else using our dax device.
if lsof "$CXL_DAX_DEVICE" 2>/dev/null | grep -v "^COMMAND"; then
   echo "[ERROR] $CXL_DAX_DEVICE is in use — pick a different one (export CXL_DAX_DEVICE=/dev/dax0.4)" >&2
   exit 1
fi

OVERALL_START=$(date +%s)
for wl in "${YCSB_WORKLOADS[@]}"; do
   for m in "${MODES[@]}"; do
      run_ycsb "$wl" "$m"
   done
done
for m in "${MODES[@]}"; do
   run_tpcc "$m"
done
OVERALL_SEC=$(( $(date +%s) - OVERALL_START ))
OVERALL_MIN=$(awk "BEGIN {printf \"%.1f\", $OVERALL_SEC / 60.0}")

build_summary

log ""
log "===== DONE in ${OVERALL_SEC}s (${OVERALL_MIN} min) ====="
log "pass = $PASS_COUNT / $TOTAL"
log "fail = $FAIL_COUNT / $TOTAL"
if [[ ${#FAILED_TESTS[@]} -gt 0 ]]; then
   log "failed:"
   for t in "${FAILED_TESTS[@]}"; do
      log "  - $t"
   done
fi

log ""
log "outputs:"
log "  processed (small): $RESULT_DIR"
log "  raw perf.data    : $PERF_DATA_DIR"
log ""
log "to free disk after analysis:  rm -rf $PERF_DATA_DIR"
