#!/usr/bin/env bash
set -euo pipefail

# =============================================================================
# Smoke: fixed hot-set ground-truth, YCSB-B vs YCSB-C 对照
# -----------------------------------------------------------------------------
# v1 smoke_fixed_hotset.sh 跑了 100% read（YCSB-C 风格），1024 个热点 92% 进 RC，
# 没复现 RC_HR 暴跌。对照实验：在同一 fixed-hotset 上叠加 5% update（YCSB-B
# 风格），看是不是 update 路径导致 invalidation 风暴 -> RC_HR 暴跌。
#
# 2x2 网格：
#                        update_ratio=0.0 (YCSB-C)    update_ratio=0.05 (YCSB-B)
#   warmup=2M            短 warmup, 100% read         短 warmup, 5% update
#   warmup=20M           长 warmup, 100% read         长 warmup, 5% update
#                        (v1 已确认 RC=92%)            (关键检验组)
#
# 期望：B 风格的 warmup=20M 这一组若 RC% 显著低于 90%，锁定 update 路径是元凶。
# =============================================================================

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
SCRIPT_DIR="/home/zhizhi.tyf/cxl-recordcache-dev/experiment_1_ycsb_full"
RESULT_DIR="$SCRIPT_DIR/optimization_step6/smoke_fixed_hotset_bc_${TIMESTAMP}"
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

HOT_KEY_COUNT=1024
HOT_RATIO=0.95
PROBE_DUMP_MAX=50

run_one() {
    local warmup_label=$1
    local warmup=$2
    local mode_label=$3       # C or B
    local update_ratio=$4     # 0.0 or 0.05
    local result_file="$RESULT_DIR/hotset_warmup${warmup_label}_${mode_label}.log"

    echo ""
    echo "============================================"
    echo "[RUN] warmup=${warmup}  mode=${mode_label}  update_ratio=${update_ratio}"
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
        --test_update_ratio="$update_ratio" \
        --test_probe_dump_max="$PROBE_DUMP_MAX" \
        --ssd_path="$SSD_PATH" \
        --trunc=true \
        --wal=true \
        2>&1 | tee "$result_file" || exit_code=$?

    local end_ts elapsed
    end_ts=$(date +%s)
    elapsed=$((end_ts - start_ts))
    echo "[TIME] warmup=${warmup} mode=${mode_label} elapsed: ${elapsed}s" | tee -a "$result_file"
    if [ "$exit_code" -ne 0 ]; then
        echo "[WARN] exit_code=$exit_code"
    fi
    echo "[INFO] cooldown 5s"
    sleep 5
}

# 2x2 grid
run_one "2M"  2000000   "C" 0.0
run_one "2M"  2000000   "B" 0.05
run_one "20M" 20000000  "C" 0.0
run_one "20M" 20000000  "B" 0.05

echo ""
echo "============================================"
echo "[DONE] smoke_fixed_hotset_bc complete."
echo "  Results in: $RESULT_DIR"
echo "============================================"

echo ""
echo "--- Quick Summary ---"
for warmup_label in 2M 20M; do
    for mode_label in C B; do
        f="$RESULT_DIR/hotset_warmup${warmup_label}_${mode_label}.log"
        [ -f "$f" ] || continue
        echo ""
        echo "[warmup=${warmup_label} mode=${mode_label}]"
        grep "PROBE_SUMMARY" "$f" | sed 's/\x1b\[[0-9;]*m//g'
        grep "HOT_SET_PROBE_SUMMARY" "$f" | sed 's/\x1b\[[0-9;]*m//g'
        grep "MEASURE_FINAL" "$f" | sed 's/\x1b\[[0-9;]*m//g'
        grep "RC State.*Final" "$f" | sed 's/\x1b\[[0-9;]*m//g'
        grep -E "DIAG_PROMOTE|DIAG_SIEVE|DIAG_CAP cur_round" "$f" | sed 's/\x1b\[[0-9;]*m//g'
    done
done
