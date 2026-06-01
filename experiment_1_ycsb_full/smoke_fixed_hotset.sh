#!/usr/bin/env bash
set -euo pipefail

# =============================================================================
# Smoke: fixed hot-set ground-truth diagnostic
# -----------------------------------------------------------------------------
# 单变量：warmup_lookups (2M / 20M)
# 1024 个固定 hot key (FNV scramble across pages)，95% 访问命中 hot set
#
# 期望：每个 hot key 在 warmup 中被访问 ~1850 次 (2M case) 或 ~18500 次 (20M case)
#       数量级远超 admission 阈值 → 理论上 1024 个全部应该在 RC 里
# 实测 probe1 RC% 越接近 100%，说明 admission/SIEVE 对热点保留得越好
# =============================================================================

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
SCRIPT_DIR="/home/zhizhi.tyf/cxl-recordcache-dev/experiment_1_ycsb_full"
RESULT_DIR="$SCRIPT_DIR/optimization_step6/smoke_fixed_hotset_${TIMESTAMP}"
mkdir -p "$RESULT_DIR"

WORKING_SET_GIB=1.0
CXL_GIB=0.8
PAYLOAD_SIZE_BYTES=100

WORKER_THREADS=4
PP_THREADS=1
CXL_PP_THREADS=1
TWO_LEVEL_ADMISSION_THREADS=1
FORWARD_EPOCH_THREAD=1
SIEVE_EVICTION_THREAD=1
RECORD_CACHE_PROMOTE_THREAD=2

MEASURE_LOOKUPS=5000000
PROGRESS_INTERVAL=500000
WARMUP_PROGRESS_INTERVAL=500000

SSD_PATH="/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_ssd"
CXL_DAX_DEVICE="/dev/dax0.2"
BUILD_DIR="/home/zhizhi.tyf/cxl-recordcache-dev/build/frontend"
BINARY="$BUILD_DIR/experiment_fixed_hotset"

SKEW_THRESHOLD_RATIO=0.08
UNIFORM_THRESHOLD_RATIO=0.45
MAX_PER_PAGE_VISITS=8000
MAX_GLOBAL_REQUESTS_WINDOW=2000000
TRIGGER_VISIT_HISTOGRAM_UPDATE_SIZE=160000

DRAM_BP=0.1
DRAM_RC=0.05

# Hot set parameters
HOT_KEY_COUNT=1024
HOT_RATIO=0.95
PROBE_DUMP_MAX=50

run_one() {
    local label=$1
    local warmup=$2
    local result_file="$RESULT_DIR/hotset_warmup${label}.log"

    echo ""
    echo "============================================"
    echo "[RUN] warmup=${warmup}  hot_keys=${HOT_KEY_COUNT}  hot_ratio=${HOT_RATIO}"
    echo "      -> $result_file"
    echo "============================================"

    local start_ts
    start_ts=$(date +%s)
    local exit_code=0
    "$BINARY" \
        --test_admission_mode=two_level \
        --cxl_tiering_enabled=true \
        --cxl_gib="$CXL_GIB" \
        --cxl_dax_device_path="$CXL_DAX_DEVICE" \
        --pp_threads="$PP_THREADS" \
        --cxl_pp_threads="$CXL_PP_THREADS" \
        --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS" \
        --delay_admission_recordcache_threads_start=true \
        --dram_buffer_pool_gib="$DRAM_BP" \
        --dram_recordcache_gib="$DRAM_RC" \
        --forward_epoch_thread="$FORWARD_EPOCH_THREAD" \
        --sieve_eviction_thread="$SIEVE_EVICTION_THREAD" \
        --record_cache_promote_thread="$RECORD_CACHE_PROMOTE_THREAD" \
        --skew_threshold_ratio="$SKEW_THRESHOLD_RATIO" \
        --uniform_threshold_ratio="$UNIFORM_THRESHOLD_RATIO" \
        --max_per_page_visits="$MAX_PER_PAGE_VISITS" \
        --max_global_requests_window="$MAX_GLOBAL_REQUESTS_WINDOW" \
        --trigger_visit_histogram_update_size="$TRIGGER_VISIT_HISTOGRAM_UPDATE_SIZE" \
        --test_working_set_gib="$WORKING_SET_GIB" \
        --test_payload_size_bytes="$PAYLOAD_SIZE_BYTES" \
        --worker_threads="$WORKER_THREADS" \
        --vi=true \
        --test_warmup_lookups="$warmup" \
        --test_measure_lookups="$MEASURE_LOOKUPS" \
        --test_warmup_progress_interval="$WARMUP_PROGRESS_INTERVAL" \
        --test_progress_interval="$PROGRESS_INTERVAL" \
        --test_hot_key_count="$HOT_KEY_COUNT" \
        --test_hot_ratio="$HOT_RATIO" \
        --test_probe_dump_max="$PROBE_DUMP_MAX" \
        --ssd_path="$SSD_PATH" \
        --trunc=true \
        --wal=true \
        2>&1 | tee "$result_file" || exit_code=$?

    local end_ts elapsed
    end_ts=$(date +%s)
    elapsed=$((end_ts - start_ts))
    echo "[TIME] warmup=${warmup} elapsed: ${elapsed}s" | tee -a "$result_file"
    if [ "$exit_code" -ne 0 ]; then
        echo "[WARN] exit_code=$exit_code"
    fi
    echo "[INFO] cooldown 5s"
    sleep 5
}

run_one "2M"  2000000
run_one "20M" 20000000

echo ""
echo "============================================"
echo "[DONE] smoke_fixed_hotset complete."
echo "  Results in: $RESULT_DIR"
echo "============================================"

echo ""
echo "--- Quick Summary ---"
for label in 2M 20M; do
    f="$RESULT_DIR/hotset_warmup${label}.log"
    [ -f "$f" ] || continue
    echo ""
    echo "[warmup=${label}]"
    grep "PROBE_SUMMARY" "$f" | sed 's/\x1b\[[0-9;]*m//g'
    grep "HOT_SET_PROBE_SUMMARY" "$f" | sed 's/\x1b\[[0-9;]*m//g'
    grep "MEASURE_FINAL" "$f" | sed 's/\x1b\[[0-9;]*m//g'
    grep "RC State.*Final" "$f" | sed 's/\x1b\[[0-9;]*m//g'
done
