#!/usr/bin/env bash
#
# Profile RecordCache hot path across ALL 4 admission modes for fair comparison.
#
# Goal: replace the hand-waved per-step ns estimates in analysis_performance.md
# with real cycle distributions, AND validate the bf-tree/hybridtier/3T
# baselines we compare against (so we can say "bf-tree hits really do cost
# X ns" instead of "I estimated 100 ns").
#
# Matrix: 4 modes × 2 workloads = 8 profile runs
#   modes:     two_level | bf-tree | hybried-tier-asplos2025 | tiered-indexing-zxj
#   workloads: YCSB-C (100R, cleanest hit-path attribution)
#              YCSB-A (50R/50W, exposes write path / SeqLock / writeback contention)
#
# Each run produces (in $RESULT_DIR):
#   - <wl>_<mode>.log              workload stdout (Mqps, RC_HR, ...)
#   - <wl>_<mode>.stat.txt         perf stat (cycles, IPC, cache miss)
#   - <wl>_<mode>.perf.data        raw perf samples
#   - <wl>_<mode>.report.txt       perf report --stdio (top funcs)
#   - <wl>_<mode>.folded           collapsed stacks
#   - <wl>_<mode>.svg              full flamegraph
#   - <wl>_<mode>.cache.svg        cache-subset flamegraph (RecordCache/mini-page/etc)
#
# Setup choices:
#   - dax0.3, isolated SSD path  — avoid colliding with running baselines
#   - --call-graph dwarf,16384   — build is -O2 -g (no frame pointers); dwarf
#                                  unwind needs a generous user-stack dump
#   - -F 997 (prime freq)        — avoid aliasing with periodic ticks
#   - warmup=5M measure=15M      — enough samples for stable ranking,
#                                  small perf.data
#

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/frontend"

# Isolation: avoid colliding with any running baseline / opt experiment.
CXL_DAX_DEVICE="${CXL_DAX_DEVICE:-/dev/dax0.3}"
SSD_PATH="${SSD_PATH:-/data1/zhizhi.tyf/cxl_test_tmp/profile_tmp}"
mkdir -p "$(dirname "$SSD_PATH")"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="$SCRIPT_DIR/profile_all_modes_${TIMESTAMP}"
mkdir -p "$RESULT_DIR"

FLAMEGRAPH_DIR="${FLAMEGRAPH_DIR:-/home/zhizhi.tyf/FlameGraph}"
if [[ ! -x "$FLAMEGRAPH_DIR/flamegraph.pl" ]]; then
   echo "[ERROR] flamegraph.pl not found at $FLAMEGRAPH_DIR" >&2
   exit 1
fi

# ----- workload config (match medium_benchmark / opt-v1 so profile reflects
#       the configuration we are actually optimizing) -----
WORKING_SET_GIB=2.0
PAYLOAD_SIZE_BYTES=100
CXL_GIB=2.0
ZIPF_THETA=0.99
DRAM_TOTAL=0.5      # total DRAM budget (BP+RC for two_level, all BP for baselines)
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
RC_SEQLOCK_RETRY_MAX=8

# Short run — enough samples for stable hot-spot ranking, perf.data <2GB
WARMUP_LOOKUPS=5000000
MEASURE_LOOKUPS=15000000
PROGRESS_INTERVAL=5000000

PERF_FREQ=997
PERF_CALLGRAPH_STACK=16384
COOLDOWN_SECONDS=8

MODES=(two_level bf-tree hybried-tier-asplos2025 tiered-indexing-zxj)
WORKLOADS=(c a)
TOTAL=$((${#MODES[@]} * ${#WORKLOADS[@]}))
CURRENT=0

log() { echo "[$(date '+%H:%M:%S')] $*"; }

mode_flags() {
   local mode="$1"
   case "$mode" in
      two_level)
         echo --test_admission_mode=two_level \
              --cxl_tiering_enabled=true --cxl_gib="$CXL_GIB" \
              --cxl_dax_device_path="$CXL_DAX_DEVICE" \
              --pp_threads="$PP_THREADS" --cxl_pp_threads="$CXL_PP_THREADS" \
              --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS" \
              --delay_admission_recordcache_threads_start=true \
              --dram_buffer_pool_gib="$DRAM_BP_TWO_LEVEL" \
              --dram_recordcache_gib="$DRAM_RC_TWO_LEVEL" \
              --forward_epoch_thread="$FORWARD_EPOCH_THREAD" \
              --sieve_eviction_thread="$SIEVE_EVICTION_THREAD" \
              --record_cache_promote_thread="$RECORD_CACHE_PROMOTE_THREAD" \
              --rc_skip_dt_id_prefix=true \
              --rc_seqlock_retry_max="$RC_SEQLOCK_RETRY_MAX"
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

# Functions to filter out for the "cache subset" flamegraph — different per mode.
cache_subset_pattern() {
   local mode="$1"
   case "$mode" in
      two_level)
         echo "RecordCache|tryLookupInRecordCache|tryWriteThroughSync|tryInvalidateForRemove|GetFromRecordCache|HashBytes|SeqLock|SieveFIFO|TwoLevelAdmission|RecordCMS|PageCMS"
         ;;
      bf-tree)
         echo "BfTree|MiniPage|mini_page|mini_hit|BufferPool|HotPageCandidates"
         ;;
      hybried-tier-asplos2025)
         echo "HybridTier|CountBloom|MomentumSketch|FourGrid|HotPageCandidates|hot_page_candidates|frequency_cbf|momentum_sketch|BufferPool"
         ;;
      tiered-indexing-zxj)
         echo "TieredIndexing|tiered_indexing|OptimisticLockTable|BufferPool|BTree"
         ;;
   esac
}

run_one() {
   local wl="$1"
   local mode="$2"

   CURRENT=$((CURRENT + 1))

   local binary="$BUILD_DIR/experiment_1_ycsb_${wl}"
   if [[ ! -x "$binary" ]]; then
      echo "[ERROR] $binary missing — skip"
      return 1
   fi

   local tag="ycsb_${wl}_${mode}"
   local stdout_log="$RESULT_DIR/${tag}.log"
   local perf_data="$RESULT_DIR/${tag}.perf.data"
   local folded="$RESULT_DIR/${tag}.folded"
   local svg="$RESULT_DIR/${tag}.svg"
   local report="$RESULT_DIR/${tag}.report.txt"
   local stat="$RESULT_DIR/${tag}.stat.txt"

   log "[$CURRENT/$TOTAL] === ${tag} ==="

   local mflags
   mflags=$(mode_flags "$mode")
   local common_args=(
      --test_zipf_theta="$ZIPF_THETA"
      --test_working_set_gib="$WORKING_SET_GIB"
      --test_payload_size_bytes="$PAYLOAD_SIZE_BYTES"
      --test_warmup_lookups="$WARMUP_LOOKUPS"
      --test_measure_lookups="$MEASURE_LOOKUPS"
      --test_progress_interval="$PROGRESS_INTERVAL"
      --worker_threads="$WORKER_THREADS"
      --vi=true --wal=true --trunc=true
      --ssd_path="$SSD_PATH"
   )

   # pass 1: perf stat
   log "  pass 1/2: perf stat"
   rm -f "$SSD_PATH"
   perf stat -e task-clock,context-switches,cycles,instructions,branches,branch-misses,cache-references,cache-misses,L1-dcache-load-misses,LLC-load-misses \
      -o "$stat" \
      "$binary" "${common_args[@]}" $mflags \
      >"${stdout_log}.stat_pass" 2>&1 || echo "[WARN] perf stat exit=$?"

   sleep "$COOLDOWN_SECONDS"

   # pass 2: perf record
   log "  pass 2/2: perf record"
   rm -f "$SSD_PATH"
   perf record -F "$PERF_FREQ" --call-graph "dwarf,$PERF_CALLGRAPH_STACK" \
      -o "$perf_data" \
      -- "$binary" "${common_args[@]}" $mflags \
      >"$stdout_log" 2>&1 || echo "[WARN] perf record exit=$?"

   if [[ ! -s "$perf_data" ]]; then
      echo "[ERROR] perf.data empty for $tag"
      sleep "$COOLDOWN_SECONDS"
      return 1
   fi

   log "  post-process: report"
   perf report -i "$perf_data" --stdio --no-children --percent-limit 0.3 \
      > "$report" 2>/dev/null || true

   log "  post-process: flamegraph"
   perf script -i "$perf_data" 2>/dev/null \
      | "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" 2>/dev/null \
      > "$folded"

   if [[ -s "$folded" ]]; then
      "$FLAMEGRAPH_DIR/flamegraph.pl" \
         --title="${mode} YCSB-${wl^^} hot path" \
         --subtitle="dax=$CXL_DAX_DEVICE, measure=${MEASURE_LOOKUPS}" \
         "$folded" > "$svg" 2>/dev/null || true
      log "  → $svg"
   else
      echo "[WARN] folded empty for $tag"
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
      log "  → $svg_sub"
   fi

   sleep "$COOLDOWN_SECONDS"
}

# Summary helper: per (mode, workload) extract Mqps + key perf-stat metrics.
build_summary() {
   local csv="$RESULT_DIR/summary.csv"
   {
      echo "workload,mode,mqps,RC_HR,ipc,cycles_billion,l1d_miss_pct,llc_miss_pct"
      for wl in "${WORKLOADS[@]}"; do
         for m in "${MODES[@]}"; do
            local log="$RESULT_DIR/ycsb_${wl}_${m}.log"
            local stat="$RESULT_DIR/ycsb_${wl}_${m}.stat.txt"
            local mqps="" rc="" ipc="" cyc="" l1miss="" llcmiss=""
            if [[ -f "$log" ]]; then
               local final
               final=$(grep -E "^.*Mqps=|mode=.*Mqps" "$log" | tail -1)
               mqps=$(echo "$final" | grep -oP 'Mqps=\K[0-9.]+' || echo "")
               rc=$(  echo "$final" | grep -oP 'RC_HR=\K[0-9.]+' || echo "")
            fi
            if [[ -f "$stat" ]]; then
               # perf stat output is somewhat free-form; do best-effort grep.
               ipc=$(grep -oP '#\s*\K[0-9.]+(?=\s+insn per cycle)' "$stat" | head -1)
               local cyc_raw
               cyc_raw=$(grep -oP '^\s*\K[0-9,]+(?=\s+cycles)' "$stat" | head -1 | tr -d ',')
               if [[ -n "$cyc_raw" ]]; then
                  cyc=$(awk -v c="$cyc_raw" 'BEGIN{printf "%.2f", c/1e9}')
               fi
               # L1-dcache-load-misses → percent of "cache-references" not available
               # but it's reported as raw count; we just emit raw billions.
               local l1_raw llc_raw
               l1_raw=$( grep -oP '^\s*\K[0-9,]+(?=\s+L1-dcache-load-misses)' "$stat" | head -1 | tr -d ',')
               llc_raw=$(grep -oP '^\s*\K[0-9,]+(?=\s+LLC-load-misses)'        "$stat" | head -1 | tr -d ',')
               if [[ -n "$l1_raw"  ]]; then l1miss=$(awk -v c="$l1_raw"  'BEGIN{printf "%.2f", c/1e9}'); fi
               if [[ -n "$llc_raw" ]]; then llcmiss=$(awk -v c="$llc_raw" 'BEGIN{printf "%.2f", c/1e9}'); fi
            fi
            echo "${wl},${m},${mqps},${rc},${ipc},${cyc},${l1miss},${llcmiss}"
         done
      done
   } > "$csv"

   echo ""
   log "summary: $csv"
   column -s, -t < "$csv"
}

# ---- main ----
log "result dir: $RESULT_DIR"
log "cxl_dax   : $CXL_DAX_DEVICE  (must NOT be in use)"
log "ssd_path  : $SSD_PATH"
log "perf      : $(perf --version 2>&1)"
log "modes     : ${MODES[*]}"
log "workloads : ${WORKLOADS[*]}"
log "total     : $TOTAL runs"

# Sanity: make sure no other process is using our dax device.
if lsof "$CXL_DAX_DEVICE" 2>/dev/null | grep -v "^COMMAND"; then
   echo "[ERROR] $CXL_DAX_DEVICE is in use — pick a different one (export CXL_DAX_DEVICE=/dev/dax0.4)" >&2
   exit 1
fi

OVERALL_START=$(date +%s)
for wl in "${WORKLOADS[@]}"; do
   for m in "${MODES[@]}"; do
      run_one "$wl" "$m"
   done
done
OVERALL_SEC=$(( $(date +%s) - OVERALL_START ))
OVERALL_MIN=$(awk "BEGIN {printf \"%.1f\", $OVERALL_SEC / 60.0}")

build_summary

log ""
log "=== DONE in ${OVERALL_SEC}s (${OVERALL_MIN} min) ==="
log "outputs: $RESULT_DIR"
log ""
log "Next:"
log "  - open *.svg in browser to inspect flamegraphs"
log "  - the .cache.svg variants are filtered to each mode's cache-layer frames"
log "  - feed summary.csv + report.txt back so I can back-fill the md files"
