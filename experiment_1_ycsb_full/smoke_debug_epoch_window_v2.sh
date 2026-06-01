#!/usr/bin/env bash
set -euo pipefail

# =============================================================================
# Smoke Debug v2: 修正 v1 用错的 flag
# -----------------------------------------------------------------------------
# v1 想用 --max_global_requests_window 控制 epoch tick 频率，但代码里那个
# 字段早已是死代码（SkewRecordAdmission.hpp:227 标注 "kept for compatibility"），
# 所以 v1 四组数据完全一样。
#
# 真正的 epoch tick trigger 在 SampledVisitHistogram::BackgroundThreadTryUpdate()
# -> sampling_set.ShouldTriggerVisitHistogramUpdate()
# -> 用 FLAGS_trigger_visit_histogram_update_size 控制采样窗大小。
#
# 这一版把对照点换成 --trigger_visit_histogram_update_size: 160000 vs 800000 (5x 长)。
# 其他参数全部保持跟 smoke_debug_hit_drop.sh 一致。
#
# 跑 2x2 网格 (warmup × trigger_size)：
#                        trigger=160K (默认)        trigger=800K (5x 长)
#   warmup=2M            短 warmup 基线              理论上变化不大
#   warmup=20M           复现的"翻车"组              关键检验组：epoch tick 应少 ~5x
# =============================================================================

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
SCRIPT_DIR="/home/zhizhi.tyf/cxl-recordcache-dev/experiment_1_ycsb_full"
RESULT_DIR="$SCRIPT_DIR/optimization_step6/smoke_debug_epoch_window_v2_${TIMESTAMP}"
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
MAX_GLOBAL_REQUESTS_WINDOW=2000000   # 死字段，传 default 即可
# TRIGGER_VISIT_HISTOGRAM_UPDATE_SIZE 由内层循环设置 (160K vs 800K)

DRAM_BP=0.1
DRAM_RC=0.05

run_one() {
    local warmup_label=$1
    local warmup=$2
    local trigger_label=$3
    local trigger=$4
    local result_file="$RESULT_DIR/ycsbb_two_level_warmup${warmup_label}_trig${trigger_label}.log"
    local binary="$BUILD_DIR/experiment_1_ycsb_b"

    echo ""
    echo "============================================"
    echo "[RUN] warmup=${warmup}  trigger_size=${trigger}  measure=${MEASURE_LOOKUPS}"
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
        --trigger_visit_histogram_update_size="$trigger" \
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
    echo "[TIME] warmup=${warmup} trigger=${trigger} elapsed: ${elapsed}s" | tee -a "$result_file"
    if [ "$exit_code" -ne 0 ]; then
        echo "[WARN] exit_code=$exit_code"
    fi
    echo "[INFO] cooldown 5s"
    sleep 5
}

# 2x2: (warmup) x (trigger_visit_histogram_update_size)
run_one "2M"  2000000   "160K" 160000
run_one "2M"  2000000   "800K" 800000
run_one "20M" 20000000  "160K" 160000
run_one "20M" 20000000  "800K" 800000

echo ""
echo "============================================"
echo "[DONE] smoke_debug_epoch_window_v2 complete."
echo "  Results in: $RESULT_DIR"
echo "============================================"

echo ""
echo "--- Quick Summary ---"
for warmup_label in 2M 20M; do
    for trigger_label in 160K 800K; do
        f="$RESULT_DIR/ycsbb_two_level_warmup${warmup_label}_trig${trigger_label}.log"
        [ -f "$f" ] || continue
        final=$(grep "mode=two_level" "$f" 2>/dev/null | tail -1 | sed 's/\x1b\[[0-9;]*m//g')
        fillrow=$(grep "RC State.*Final" "$f" 2>/dev/null | tail -1 | sed 's/\x1b\[[0-9;]*m//g')
        sieve=$(grep "RC SIEVE evicted" "$f" 2>/dev/null | tail -1 | sed 's/\x1b\[[0-9;]*m//g')
        epoch_count=$(grep -c "admission_epoch_tick" "$f" 2>/dev/null || echo 0)
        last_warm=$(grep "skew_promote.*timeout_removed" "$f" 2>/dev/null | grep "Warmup\] " | tail -1 | sed 's/\x1b\[[0-9;]*m//g' | grep -oE "skew_promote=[0-9]+ uniform_promote=[0-9]+ timeout_removed=[0-9]+")
        echo ""
        echo "[warmup=${warmup_label} trigger=${trigger_label}]"
        echo "  final        : $final"
        echo "  fill         : $fillrow"
        echo "  sieve        : $sieve"
        echo "  epoch_ticks  : $epoch_count"
        echo "  warmup_tail  : $last_warm"
    done
done
