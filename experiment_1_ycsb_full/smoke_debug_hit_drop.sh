#!/usr/bin/env bash
set -euo pipefail

# =============================================================================
# Smoke Debug: 复现"warmup 拉长 -> RC_HR 反向下降"
# -----------------------------------------------------------------------------
# 第一版 smoke_warmup_diag.sh 没复现出来，原因是 RC/ws 比例太宽（25%），
# cache 根本没饱和，admission 抖动打不到命中率上。
#
# 这一版把 RC 从 0.25 GiB 砍到 0.05 GiB，让 cache 紧绷：
#   RC / ws = 0.05 / 1.0 = 5%     (生产是 0.5/16 = 3.1%，量级一致)
#   RC capacity ≈ 432K entries    总记录 ~3.67M -> 命中率上限被压到与生产同档
#   warmup_lookups / RC_cap       2M -> ~5×, 20M -> ~46× (生产 100M/4.3M ≈ 23×)
# 其余参数都跟 smoke_warmup_diag.sh 保持一致，单变量隔离。
#
# 跑两组：warmup=2M / 20M, measure=5M。
# =============================================================================

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
SCRIPT_DIR="/home/zhizhi.tyf/cxl-recordcache-dev/experiment_1_ycsb_full"
RESULT_DIR="$SCRIPT_DIR/optimization_step6/smoke_debug_hit_drop_${TIMESTAMP}"
mkdir -p "$RESULT_DIR"

WORKING_SET_GIB=1.0
CXL_GIB=0.8
PAYLOAD_SIZE_BYTES=100
THETA=0.90

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

SKEW_THRESHOLD_RATIO=0.08
UNIFORM_THRESHOLD_RATIO=0.45
MAX_PER_PAGE_VISITS=8000
MAX_GLOBAL_REQUESTS_WINDOW=2000000
TRIGGER_VISIT_HISTOGRAM_UPDATE_SIZE=160000

# 关键改动：RC 由 0.25 -> 0.05，让 cache 紧绷接近生产配比
DRAM_BP=0.1
DRAM_RC=0.05

run_one() {
    local label=$1
    local warmup=$2
    local result_file="$RESULT_DIR/ycsbb_two_level_warmup${label}.log"
    local binary="$BUILD_DIR/experiment_1_ycsb_b"

    echo ""
    echo "============================================"
    echo "[RUN] warmup=${warmup}  measure=${MEASURE_LOOKUPS}"
    echo "      DRAM_RC=${DRAM_RC} GiB  (RC/ws ≈ $(awk "BEGIN{printf \"%.1f%%\", ${DRAM_RC}/${WORKING_SET_GIB}*100}"))"
    echo "      -> $result_file"
    echo "============================================"

    local start_ts
    start_ts=$(date +%s)
    local exit_code=0
    "$binary" \
        --test_admission_mode=two_level \
        --test_zipf_theta="$THETA" \
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
        --ssd_path="$SSD_PATH" \
        --trunc=true \
        --wal=true \
        2>&1 | tee "$result_file" || exit_code=$?

    local end_ts elapsed
    end_ts=$(date +%s)
    elapsed=$((end_ts - start_ts))
    echo "[TIME] warmup=${warmup} elapsed: ${elapsed}s" | tee -a "$result_file"
    if [ "$exit_code" -ne 0 ]; then
        echo "[WARN] exit_code=$exit_code (warmup=${warmup})"
    fi
    echo "[INFO] cooldown 5s"
    sleep 5
}

run_one "2M"  2000000
run_one "20M" 20000000

echo ""
echo "============================================"
echo "[DONE] smoke_debug_hit_drop complete."
echo "  Results in: $RESULT_DIR"
echo "============================================"

echo ""
echo "--- Quick Summary (会被 Claude 进一步分析) ---"
for label in 2M 20M; do
    f="$RESULT_DIR/ycsbb_two_level_warmup${label}.log"
    [ -f "$f" ] || continue
    final=$(grep "mode=two_level" "$f" 2>/dev/null | tail -1 | sed 's/\x1b\[[0-9;]*m//g')
    fillrow=$(grep "RC State.*Final" "$f" 2>/dev/null | tail -1 | sed 's/\x1b\[[0-9;]*m//g')
    sieve=$(grep "RC SIEVE evicted" "$f" 2>/dev/null | tail -1 | sed 's/\x1b\[[0-9;]*m//g')
    epoch_count=$(grep -c "admission_epoch_tick" "$f" 2>/dev/null || echo 0)
    last_warm=$(grep "skew_promote.*timeout_removed" "$f" 2>/dev/null | grep "Warmup" | tail -1 | sed 's/\x1b\[[0-9;]*m//g')
    echo ""
    echo "[warmup=${label}]"
    echo "  final        : $final"
    echo "  fill         : $fillrow"
    echo "  sieve        : $sieve"
    echo "  epoch_ticks  : $epoch_count"
    echo "  warmup_tail  : $last_warm"
done
