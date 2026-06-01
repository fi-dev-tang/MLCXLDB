#!/usr/bin/env bash
# =============================================================================
# fast_start_ycsb_c_test.sh
#
# Quick-iteration version of start_ycsb_c_test.sh for rapid validation.
# Runs the same four modes but with reduced lookup counts:
#   WARMUP_LOOKUPS  : 5 000 000  (vs 20 000 000 in the full run)
#   MEASURE_LOOKUPS : 20 000 000 (vs 100 000 000 in the full run)
#
# Modes:
#   1. two_level  — full system (DRAM buffer pool + CXL + RecordCache)
#   2. page_only  — two-level admission, no RecordCache
#   3. lru        — pure LRU page tiering baseline (DRAM + CXL)
#   4. dram_ssd   — pure DRAM + SSD, no CXL buffer pool
#
# Results are written to: ycsb_ablation_study/workload_c/
#
# Usage:
#   cd /home/zhizhi.tyf/cxl-recordcache-dev
#   bash ycsb_ablation_study/workload_c/fast_start_ycsb_c_test.sh
# =============================================================================

set -euo pipefail

# =============================================================================
# ★ Tune these before each experiment run ★
# =============================================================================

# Working set size (GiB) — adjust per experiment
WORKING_SET_GIB=8.0

# Payload size per record (bytes, 50–500); key is always 8 B
PAYLOAD_SIZE_BYTES=100

# Zipfian skew (YCSB default 0.99)
ZIPF_THETA=0.99

# CXL pool size (GiB)
CXL_GIB=2.0

# DRAM budget split:
#   two_level: buffer_pool=DRAM_BP_TWO_LEVEL + record_cache=DRAM_RC_TWO_LEVEL
#   page_only / lru / dram_ssd: buffer_pool=DRAM_BP_PAGE_ONLY + record_cache=0
# Keep (DRAM_BP_TWO_LEVEL + DRAM_RC_TWO_LEVEL) == DRAM_BP_PAGE_ONLY for a
# fair comparison (same total DRAM budget across all four modes).
DRAM_BP_TWO_LEVEL=0.125
DRAM_RC_TWO_LEVEL=0.5
DRAM_BP_PAGE_ONLY=0.625   # = DRAM_BP_TWO_LEVEL + DRAM_RC_TWO_LEVEL
DRAM_BASELINE=2.625       # = DRAM_BP_TWO_LEVEL + CXL_GIB_TWO_LEVEL

# Worker / background threads
WORKER_THREADS=4
# --pp_threads controls:
#   - dramPageProviderThread count when CXL tiering is enabled
#   - pageProviderThread count when CXL tiering is disabled
PP_THREADS=1
CXL_PP_THREADS=1        # cxlPageProviderThread count (only when CXL tiering is enabled)
TWO_LEVEL_ADMISSION_THREADS=1
FORWARD_EPOCH_THREAD=1
SIEVE_EVICTION_THREAD=1
RECORD_CACHE_PROMOTE_THREAD=1

# Lookup counts — reduced for fast iteration
WARMUP_LOOKUPS=5000000
MEASURE_LOOKUPS=10000000
PROGRESS_INTERVAL=100000 

# Fixed paths
# REPO_ROOT is resolved relative to this script's location, so the script can
# be invoked from any working directory.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BINARY="$REPO_ROOT/build/frontend/ycsb_c_test"
SSD_PATH="/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_comparison"
CXL_DAX_DEVICE="/dev/dax0.2"
RESULT_DIR="$SCRIPT_DIR/monitor_result8G"

# =============================================================================
# Helpers
# =============================================================================

TIMESTAMP=$(date +%Y%m%d_%H%M%S)

log_phase() {
    echo ""
    echo "========================================"
    echo "  $1"
    echo "========================================"
}

check_binary() {
    if [[ ! -x "$BINARY" ]]; then
        echo "[ERROR] Binary not found or not executable: $BINARY"
        echo "        Please build first: cmake --build build --target ycsb_c_test"
        exit 1
    fi
}

mkdir -p "$RESULT_DIR"

# =============================================================================
# Common flags shared across all modes
# =============================================================================
COMMON_FLAGS=(
    --test_working_set_gib="$WORKING_SET_GIB"
    --test_payload_size_bytes="$PAYLOAD_SIZE_BYTES"
    --test_zipf_theta="$ZIPF_THETA"
    --worker_threads="$WORKER_THREADS"
    --vi=true
    --test_warmup_lookups="$WARMUP_LOOKUPS"
    --test_measure_lookups="$MEASURE_LOOKUPS"
    --test_progress_interval="$PROGRESS_INTERVAL"
    --ssd_path="$SSD_PATH"
    --trunc=true
    --wal=true
)

# Extra flags used only by CXL-enabled modes (two_level / page_only / lru).
# --pp_threads controls dramPageProviderThread count when CXL is enabled.
# --cxl_pp_threads controls cxlPageProviderThread count.
CXL_FLAGS=(
    --cxl_tiering_enabled=true
    --cxl_gib="$CXL_GIB"
    --cxl_dax_device_path="$CXL_DAX_DEVICE"
    --pp_threads="$PP_THREADS"
    --cxl_pp_threads="$CXL_PP_THREADS"
    --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS"
    --delay_admission_recordcache_threads_start=true
)

# =============================================================================
# Run helper: executes one mode and tees output to result file
#
# Usage:
#   run_mode <label> <admission_mode> [--extra_flag=value ...]
#
# <label>          : used for the result file name (e.g. "dram_ssd")
# <admission_mode> : passed as --test_admission_mode (e.g. "page_only")
#
# The caller is responsible for appending CXL_FLAGS when CXL is needed.
# =============================================================================
run_mode() {
    local label="$1"
    local mode="$2"
    local result_file="$RESULT_DIR/fast_result_ycsbc_${label}_ws${WORKING_SET_GIB}gib_${TIMESTAMP}.csv"
    shift 2
    local extra_flags=("$@")

    log_phase "Starting mode: $mode"
    echo "[INFO] Result file: $result_file"
    echo "[INFO] Working set: ${WORKING_SET_GIB} GiB  |  CXL: ${CXL_GIB} GiB  |  theta: ${ZIPF_THETA}"
    echo "[INFO] [FAST MODE] warmup=${WARMUP_LOOKUPS}  measure=${MEASURE_LOOKUPS}"

    local start_ts
    start_ts=$(date +%s)

    # Build the command-line string once so it can be written to both stdout
    # and the result file as a self-contained log header.
    local cmd_header
    cmd_header="$(
        echo ""
        echo "[CMD] $BINARY \\"
        echo "    --test_admission_mode=$mode \\"
        for flag in "${extra_flags[@]}"; do echo "    $flag \\"; done
        for flag in "${COMMON_FLAGS[@]}"; do echo "    $flag \\"; done
        echo ""
    )"

    # Print the command header to stdout immediately (before tee starts).
    echo "$cmd_header"

    # Run the binary. Use a subshell so that a segfault during LeanStore
    # teardown (exit code 139) is treated as a non-fatal warning rather than
    # aborting the whole ablation run.  The measured data is already flushed
    # to the result file before teardown, so the results are still valid.
    # The command header is prepended inside the pipeline so it also appears
    # at the top of the result CSV file.
    local exit_code=0
    {
        echo "$cmd_header"
        "$BINARY" \
            --test_admission_mode="$mode" \
            "${extra_flags[@]}" \
            "${COMMON_FLAGS[@]}" \
            2>&1
    } | tee "$result_file" || exit_code=$?

    if [[ $exit_code -ne 0 ]]; then
        echo ""
        echo "[WARN] Binary exited with code $exit_code (likely a teardown segfault)."
        echo "[WARN] Measured data is already written — results in $result_file are valid."
    fi

    # Wait a moment to ensure the OS has fully released the DAX device,
    # SSD file handles, and shared memory before the next mode starts.
    echo "[INFO] Waiting 20 s for full process teardown before next mode..."
    sleep 20

    local end_ts elapsed
    end_ts=$(date +%s)
    elapsed=$((end_ts - start_ts))
    echo ""
    echo "[INFO] Mode '$mode' finished in ${elapsed}s. Result: $result_file"
}

# =============================================================================
# Pre-flight check
# =============================================================================
check_binary

echo ""
echo "============================================"
echo "  YCSB-C Ablation Study  [FAST MODE]"
echo "  working_set_gib    = $WORKING_SET_GIB"
echo "  payload_size_bytes = $PAYLOAD_SIZE_BYTES B"
echo "  cxl_gib            = $CXL_GIB GiB"
echo "  warmup_lookups     = $WARMUP_LOOKUPS"
echo "  measure_lookups    = $MEASURE_LOOKUPS"
echo "  timestamp          = $TIMESTAMP"
echo "============================================"

# =============================================================================
# Run 1: two_level — full system (DRAM buffer pool + CXL + RecordCache)
# =============================================================================
run_mode "two_level" "two_level" \
    "${CXL_FLAGS[@]}" \
    --dram_buffer_pool_gib="$DRAM_BP_TWO_LEVEL" \
    --dram_recordcache_gib="$DRAM_RC_TWO_LEVEL" \
    --forward_epoch_thread="$FORWARD_EPOCH_THREAD" \
    --sieve_eviction_thread="$SIEVE_EVICTION_THREAD" \
    --record_cache_promote_thread="$RECORD_CACHE_PROMOTE_THREAD"

# =============================================================================
# Run 2: page_only — two-level admission, no RecordCache
# =============================================================================
run_mode "page_only" "page_only" \
    "${CXL_FLAGS[@]}" \
    --dram_buffer_pool_gib="$DRAM_BP_PAGE_ONLY"

# =============================================================================
# Run 3: lru — pure LRU page tiering baseline (DRAM + CXL)
# =============================================================================
run_mode "lru" "lru" \
    "${CXL_FLAGS[@]}" \
    --dram_buffer_pool_gib="$DRAM_BP_PAGE_ONLY"

# =============================================================================
# Run 4: dram_ssd — pure DRAM + SSD, no CXL buffer pool.
# Uses page_only admission mode with CXL disabled.
# No CXL-related flags or background threads (no dram_pp_threads,
# cxl_pp_threads, two_level_admission_threads, sieve_eviction_thread,
# forward_epoch_thread, record_cache_promote_thread).
# Uses plain pp_threads for the standard DRAM buffer pool page promoter.
# =============================================================================
run_mode "dram_ssd" "page_only" \
    --cxl_tiering_enabled=false \
    --dram_buffer_pool_gib="$DRAM_BASELINE" \
    --pp_threads="$PP_THREADS"

# =============================================================================
# Done
# =============================================================================
log_phase "All four modes completed  [FAST MODE]"
echo "[INFO] Results saved in: $RESULT_DIR"
ls -lh "$RESULT_DIR"/fast_result_ycsbc_*_ws${WORKING_SET_GIB}gib_${TIMESTAMP}.csv 2>/dev/null || true
