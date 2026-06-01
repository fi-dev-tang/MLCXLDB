#!/usr/bin/env bash
set -euo pipefail

# =============================================================================
# Smoke Test: Two-Level, YCSB-A only (最小规模验跑通)
# working_set=0.5GiB, CXL=0.4GiB, warmup=2M, measure=5M
# =============================================================================

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
SCRIPT_DIR="/home/zhizhi.tyf/cxl-recordcache-dev/experiment_1_ycsb_full"
RESULT_DIR="$SCRIPT_DIR/optimization_step6/smoke_test_${TIMESTAMP}"
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

WARMUP_LOOKUPS=5000000
MEASURE_LOOKUPS=10000000
PROGRESS_INTERVAL=1000000
WARMUP_PROGRESS_INTERVAL=1000000

SSD_PATH="/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_ssd"
CXL_DAX_DEVICE="/dev/dax0.2"
BUILD_DIR="/home/zhizhi.tyf/cxl-recordcache-dev/build/frontend"

SKEW_THRESHOLD_RATIO=0.08
UNIFORM_THRESHOLD_RATIO=0.45
MAX_PER_PAGE_VISITS=8000
MAX_GLOBAL_REQUESTS_WINDOW=2000000
TRIGGER_VISIT_HISTOGRAM_UPDATE_SIZE=160000

get_dram_bp() {
    local wl=$1
    case "$wl" in
        a|b|c|f) echo "0.1"  ;;
        d|e)     echo "0.25" ;;
    esac
}

get_dram_rc() {
    local wl=$1
    case "$wl" in
        a|b|c|f) echo "0.25" ;;
        d|e)     echo "0.1"  ;;
    esac
}

echo "============================================"
echo "  Smoke Test: Two-Level, theta=$THETA"
echo "  working_set=${WORKING_SET_GIB}GiB, CXL=${CXL_GIB}GiB"
echo "  warmup=${WARMUP_LOOKUPS}, measure=${MEASURE_LOOKUPS}"
echo "  Result dir: $RESULT_DIR"
echo "============================================"

# 先只跑 a c 验证流程
for wl in a b c d e f; do
    DRAM_BP=$(get_dram_bp "$wl")
    DRAM_RC=$(get_dram_rc "$wl")
    binary="$BUILD_DIR/experiment_1_ycsb_${wl}"
    result_file="$RESULT_DIR/ycsb${wl}_two_level_theta${THETA}.log"

    echo ""
    echo "[RUN] YCSB-${wl^^} | dram_bp=${DRAM_BP}GiB, rc=${DRAM_RC}GiB"
    echo "      -> $result_file"

    wl_start_ts=$(date +%s)
    exit_code=0
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
        --test_warmup_lookups="$WARMUP_LOOKUPS" \
        --test_measure_lookups="$MEASURE_LOOKUPS" \
        --test_warmup_progress_interval="$WARMUP_PROGRESS_INTERVAL" \
        --test_progress_interval="$PROGRESS_INTERVAL" \
        --ssd_path="$SSD_PATH" \
        --trunc=true \
        --wal=true \
        2>&1 | tee "$result_file" || exit_code=$?

    wl_end_ts=$(date +%s)
    wl_elapsed_sec=$((wl_end_ts - wl_start_ts))
    wl_elapsed_min=$(awk "BEGIN {printf \"%.2f\", $wl_elapsed_sec / 60.0}")

    if [ "$exit_code" -ne 0 ]; then
        echo "[WARN] YCSB-${wl^^} exit code $exit_code"
        echo "[WARN] 检查 log: $result_file"
    else
        echo "[OK]   YCSB-${wl^^} 完成"
    fi
    echo "[TIME] YCSB-${wl^^} elapsed: ${wl_elapsed_sec}s (${wl_elapsed_min} min)" | tee -a "$result_file"

    echo "[INFO] cooldown 5s"
    sleep 5
done

echo ""
echo "============================================"
echo "[DONE] Smoke test complete."
echo "  Results in: $RESULT_DIR"
echo "============================================"

echo ""
echo "--- Summary ---"
for wl in a b c d e f; do
    f="$RESULT_DIR/ycsb${wl}_two_level_theta${THETA}.log"
    if [ -f "$f" ]; then
        summary=$(grep "mode=two_level" "$f" 2>/dev/null | tail -1)
        if [ -n "$summary" ]; then
            mqps=$(echo "$summary" | grep -oP 'Mqps=[\d.]+')
            rchr=$(echo "$summary" | grep -oP 'RC_HR=[\d.]+%')
            p99=$(echo "$summary" | grep -oP 'p99_us=[\d.]+')
            echo "  YCSB-${wl^^}: $mqps  $rchr  $p99"
        else
            echo "  YCSB-${wl^^}: NO SUMMARY LINE (grep 'mode=two_level' 看log)"
        fi
        # RC fill ratio (final)
        rc_final=$(grep "RC State.*Final" "$f" 2>/dev/null | grep -oP 'fill=[\d.]+%')
        # Warmup timing
        warmup_line=$(grep "warmup_elapsed" "$f" 2>/dev/null | tail -1)
        warmup_t=$(echo "$warmup_line" | grep -oP 'warmup_elapsed=[\d.]+s')
        warmup_r=$(echo "$warmup_line" | grep -oP 'warmup_ratio=[\d.]+%')
        [ -n "$rc_final" ] && echo "         RC_fill_final: $rc_final"
        [ -n "$warmup_t" ] && echo "         $warmup_t  $warmup_r"
    fi
done
