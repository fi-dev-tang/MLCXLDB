#!/usr/bin/env bash

set -uo pipefail

# =============================================================================
# 5/24 22:00 retry of the 4 still-failing tests after D7 adapter fix.
#
# Failures to retry:
#   - D1   two_level tpcc        : 5/23 glibc malloc assert (not retested today)
#   - D6-bf bf-tree YCSB-E       : init race, Phase 1 "Error opening counter L1-miss" hang
#   - D7   hybridtier YCSB-D     : *fixed by removing ensure(OK) from LeanStoreAdapter.hpp:105*
#                                  workload counts not_founds itself; YCSB-D race window
#                                  between next_insert_key.fetch_add and btree insert commit
#                                  let a lookup pick a key not yet present.
#   - D8   bf-tree YCSB-F        : 98% warmup worker 1 ready hang (new mode)
#
# PREREQ: rebuild after the LeanStoreAdapter.hpp edit:
#   cd /home/zhizhi.tyf/cxl-WT-comparison/build && cmake --build . -j
#
# Run sequentially (different DAX devices but easier to read logs). Each on
# its own SSD file/CXL DAX so a hang in one doesn't poison the next.
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/frontend"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="$SCRIPT_DIR/bench_retry_pending_${TIMESTAMP}"
mkdir -p "$RESULT_DIR"

# Common workload params (match run_bf_repro.sh / run_hybridtier_repro.sh)
WORKING_SET_GIB=2.0
PAYLOAD_SIZE_BYTES=100
CXL_GIB=2.0
ZIPF_THETA=0.99
WORKER_THREADS=8
PP_THREADS=1
CXL_PP_THREADS=1
WARMUP_LOOKUPS=20000000
MEASURE_LOOKUPS=30000000
PROGRESS_INTERVAL=5000000
TPCC_WAREHOUSE_COUNT=10
TPCC_WARMUP_SECONDS=20
TPCC_MEASURE_SECONDS=40
COOLDOWN_SECONDS=10

# DRAM splits (match medium_benchmark / *_repro)
DRAM_BP_TWO_LEVEL=0.125
DRAM_RC_TWO_LEVEL=0.375
DRAM_BP_BASELINE=0.5

# two_level needs the same extra threads as run_medium_benchmark.sh
TWO_LEVEL_ADMISSION_THREADS=2
FORWARD_EPOCH_THREAD=1
SIEVE_EVICTION_THREAD=1
RECORD_CACHE_PROMOTE_THREAD=4

log_phase() {
   echo ""
   echo "============================================================"
   echo "  $1"
   echo "  $(date '+%Y-%m-%d %H:%M:%S')"
   echo "============================================================"
}
log_info() { echo "[INFO] $(date '+%H:%M:%S') $1"; }

PASS_COUNT=0
FAIL_COUNT=0
declare -a FAILED_TESTS=()

mark_pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "[PASS] $1"; }
mark_fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); FAILED_TESTS+=("$1"); echo "[FAIL] $1"; }

# -----------------------------------------------------------------------------
# 1. D6-bf : bf-tree YCSB-E (timing self-heal expected)
# -----------------------------------------------------------------------------
run_bf_e() {
   local binary="$BUILD_DIR/experiment_1_ycsb_e"
   local out="$RESULT_DIR/D6bf_ycsb_e_bf-tree.log"
   local timeout_s=1800

   log_phase "[1/6] D6-bf : bf-tree YCSB-E (timeout=${timeout_s}s)"
   local start_ts=$(date +%s) exit_code=0

   timeout --signal=KILL "$timeout_s" "$binary" \
      --test_zipf_theta="$ZIPF_THETA" \
      --test_working_set_gib="$WORKING_SET_GIB" \
      --test_payload_size_bytes="$PAYLOAD_SIZE_BYTES" \
      --test_warmup_lookups=10000000 \
      --test_measure_lookups="$MEASURE_LOOKUPS" \
      --test_progress_interval="$PROGRESS_INTERVAL" \
      --worker_threads="$WORKER_THREADS" \
      --vi=true --wal=true --trunc=true \
      --ssd_path=/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_retry_bf_e \
      --test_admission_mode=bf-tree \
      --cxl_tiering_enabled=true --cxl_gib="$CXL_GIB" \
      --cxl_dax_device_path=/dev/dax0.2 \
      --pp_threads="$PP_THREADS" --cxl_pp_threads="$CXL_PP_THREADS" \
      --dram_buffer_pool_gib="$DRAM_BP_BASELINE" \
      --dram_recordcache_gib=0.0 \
      2>&1 | tee "$out" || exit_code=$?

   local elapsed=$(( $(date +%s) - start_ts ))
   if [[ "$exit_code" -eq 0 ]]; then mark_pass "D6-bf bf-tree YCSB-E (${elapsed}s)"
   else                              mark_fail "D6-bf bf-tree YCSB-E (exit=${exit_code}, ${elapsed}s)"
   fi
   sleep "$COOLDOWN_SECONDS"
}

# -----------------------------------------------------------------------------
# 2. D8 : bf-tree YCSB-F (98% warmup hang)
# -----------------------------------------------------------------------------
run_bf_f() {
   local binary="$BUILD_DIR/experiment_1_ycsb_f"
   local out="$RESULT_DIR/D8_ycsb_f_bf-tree.log"
   local timeout_s=2400

   log_phase "[2/6] D8 : bf-tree YCSB-F (timeout=${timeout_s}s)"
   local start_ts=$(date +%s) exit_code=0

   timeout --signal=KILL "$timeout_s" "$binary" \
      --test_zipf_theta="$ZIPF_THETA" \
      --test_working_set_gib="$WORKING_SET_GIB" \
      --test_payload_size_bytes="$PAYLOAD_SIZE_BYTES" \
      --test_warmup_lookups="$WARMUP_LOOKUPS" \
      --test_measure_lookups="$MEASURE_LOOKUPS" \
      --test_progress_interval="$PROGRESS_INTERVAL" \
      --worker_threads="$WORKER_THREADS" \
      --vi=true --wal=true --trunc=true \
      --ssd_path=/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_retry_bf_f \
      --test_admission_mode=bf-tree \
      --cxl_tiering_enabled=true --cxl_gib="$CXL_GIB" \
      --cxl_dax_device_path=/dev/dax0.2 \
      --pp_threads="$PP_THREADS" --cxl_pp_threads="$CXL_PP_THREADS" \
      --dram_buffer_pool_gib="$DRAM_BP_BASELINE" \
      --dram_recordcache_gib=0.0 \
      2>&1 | tee "$out" || exit_code=$?

   local elapsed=$(( $(date +%s) - start_ts ))
   if [[ "$exit_code" -eq 0 ]]; then mark_pass "D8 bf-tree YCSB-F (${elapsed}s)"
   else                              mark_fail "D8 bf-tree YCSB-F (exit=${exit_code}, ${elapsed}s)"
   fi
   sleep "$COOLDOWN_SECONDS"
}

# -----------------------------------------------------------------------------
# 3. D7 : hybridtier YCSB-D (validates the LeanStoreAdapter::lookup1 fix)
# -----------------------------------------------------------------------------
run_ht_d() {
   local binary="$BUILD_DIR/experiment_1_ycsb_d"
   local out="$RESULT_DIR/D7_ycsb_d_hybridtier.log"
   local timeout_s=900

   log_phase "[3/6] D7 : hybridtier YCSB-D (timeout=${timeout_s}s, post-adapter-fix)"
   local start_ts=$(date +%s) exit_code=0

   timeout --signal=KILL "$timeout_s" "$binary" \
      --test_zipf_theta="$ZIPF_THETA" \
      --test_working_set_gib="$WORKING_SET_GIB" \
      --test_payload_size_bytes="$PAYLOAD_SIZE_BYTES" \
      --test_warmup_lookups=10000000 \
      --test_measure_lookups="$MEASURE_LOOKUPS" \
      --test_progress_interval="$PROGRESS_INTERVAL" \
      --worker_threads="$WORKER_THREADS" \
      --vi=true --wal=true --trunc=true \
      --ssd_path=/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_retry_ht_d \
      --test_admission_mode=hybried-tier-asplos2025 \
      --cxl_tiering_enabled=true --cxl_gib="$CXL_GIB" \
      --cxl_dax_device_path=/dev/dax0.3 \
      --pp_threads="$PP_THREADS" --cxl_pp_threads="$CXL_PP_THREADS" \
      --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS" \
      --delay_admission_recordcache_threads_start=true \
      --dram_buffer_pool_gib="$DRAM_BP_BASELINE" \
      --dram_recordcache_gib=0.0 \
      2>&1 | tee "$out" || exit_code=$?

   local elapsed=$(( $(date +%s) - start_ts ))
   if [[ "$exit_code" -eq 0 ]]; then mark_pass "D7 hybridtier YCSB-D (${elapsed}s)"
   else                              mark_fail "D7 hybridtier YCSB-D (exit=${exit_code}, ${elapsed}s)"
   fi
   sleep "$COOLDOWN_SECONDS"
}

# -----------------------------------------------------------------------------
# 4. D1 : two_level tpcc (5/23 glibc malloc assert — confirm flaky vs stable)
# -----------------------------------------------------------------------------
run_tl_tpcc() {
   local binary="$BUILD_DIR/tpcc_compare_test"
   local out="$RESULT_DIR/D1_tpcc_two_level.log"
   local timeout_s=600

   log_phase "[4/6] D1 : two_level TPC-C (timeout=${timeout_s}s)"
   local start_ts=$(date +%s) exit_code=0

   timeout --signal=KILL "$timeout_s" "$binary" \
      --tpcc_warehouse_count="$TPCC_WAREHOUSE_COUNT" \
      --test_warmup_seconds="$TPCC_WARMUP_SECONDS" \
      --test_measure_seconds="$TPCC_MEASURE_SECONDS" \
      --test_load_data=true \
      --worker_threads="$WORKER_THREADS" \
      --vi=true --wal=true --trunc=true \
      --ssd_path=/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_retry_tl_tpcc \
      --test_admission_mode=two_level \
      --cxl_tiering_enabled=true --cxl_gib="$CXL_GIB" \
      --cxl_dax_device_path=/dev/dax0.1 \
      --pp_threads="$PP_THREADS" --cxl_pp_threads="$CXL_PP_THREADS" \
      --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS" \
      --delay_admission_recordcache_threads_start=true \
      --dram_buffer_pool_gib="$DRAM_BP_TWO_LEVEL" \
      --dram_recordcache_gib="$DRAM_RC_TWO_LEVEL" \
      --forward_epoch_thread="$FORWARD_EPOCH_THREAD" \
      --sieve_eviction_thread="$SIEVE_EVICTION_THREAD" \
      --record_cache_promote_thread="$RECORD_CACHE_PROMOTE_THREAD" \
      2>&1 | tee "$out" || exit_code=$?

   local elapsed=$(( $(date +%s) - start_ts ))
   if [[ "$exit_code" -eq 0 ]]; then mark_pass "D1 two_level TPC-C (${elapsed}s)"
   else                              mark_fail "D1 two_level TPC-C (exit=${exit_code}, ${elapsed}s)"
   fi
   sleep "$COOLDOWN_SECONDS"
}

# -----------------------------------------------------------------------------
# 5. D2 : bf-tree YCSB-A (3 prior fails; cheap repro while waiting)
# -----------------------------------------------------------------------------
run_bf_a() {
   local binary="$BUILD_DIR/experiment_1_ycsb_a"
   local out="$RESULT_DIR/D2_ycsb_a_bf-tree.log"
   local timeout_s=2400

   log_phase "[5/6] D2 : bf-tree YCSB-A (timeout=${timeout_s}s, low expectation)"
   local start_ts=$(date +%s) exit_code=0

   timeout --signal=KILL "$timeout_s" "$binary" \
      --test_zipf_theta="$ZIPF_THETA" \
      --test_working_set_gib="$WORKING_SET_GIB" \
      --test_payload_size_bytes="$PAYLOAD_SIZE_BYTES" \
      --test_warmup_lookups="$WARMUP_LOOKUPS" \
      --test_measure_lookups="$MEASURE_LOOKUPS" \
      --test_progress_interval="$PROGRESS_INTERVAL" \
      --worker_threads="$WORKER_THREADS" \
      --vi=true --wal=true --trunc=true \
      --ssd_path=/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_retry_bf_a \
      --test_admission_mode=bf-tree \
      --cxl_tiering_enabled=true --cxl_gib="$CXL_GIB" \
      --cxl_dax_device_path=/dev/dax0.2 \
      --pp_threads="$PP_THREADS" --cxl_pp_threads="$CXL_PP_THREADS" \
      --dram_buffer_pool_gib="$DRAM_BP_BASELINE" \
      --dram_recordcache_gib=0.0 \
      2>&1 | tee "$out" || exit_code=$?

   local elapsed=$(( $(date +%s) - start_ts ))
   if [[ "$exit_code" -eq 0 ]]; then mark_pass "D2 bf-tree YCSB-A (${elapsed}s)"
   else                              mark_fail "D2 bf-tree YCSB-A (exit=${exit_code}, ${elapsed}s)"
   fi
   sleep "$COOLDOWN_SECONDS"
}

# -----------------------------------------------------------------------------
# 6. bf-tree TPC-C : added 5/25 after D2/D8 payload_length fix.
#    Same binary as D1 (tpcc_compare_test) but bf-tree admission mode,
#    baseline DRAM split, no two_level threads. Uses dax0.2 to avoid
#    colliding with the two_level run on dax0.1.
# -----------------------------------------------------------------------------
run_bf_tpcc() {
   local binary="$BUILD_DIR/tpcc_compare_test"
   local out="$RESULT_DIR/D_bf_tpcc_bf-tree.log"
   local timeout_s=900

   log_phase "[6/6] bf-tree TPC-C (timeout=${timeout_s}s)"
   local start_ts=$(date +%s) exit_code=0

   timeout --signal=KILL "$timeout_s" "$binary" \
      --tpcc_warehouse_count="$TPCC_WAREHOUSE_COUNT" \
      --test_warmup_seconds="$TPCC_WARMUP_SECONDS" \
      --test_measure_seconds="$TPCC_MEASURE_SECONDS" \
      --test_load_data=true \
      --worker_threads="$WORKER_THREADS" \
      --vi=true --wal=true --trunc=true \
      --ssd_path=/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_retry_bf_tpcc \
      --test_admission_mode=bf-tree \
      --cxl_tiering_enabled=true --cxl_gib="$CXL_GIB" \
      --cxl_dax_device_path=/dev/dax0.2 \
      --pp_threads="$PP_THREADS" --cxl_pp_threads="$CXL_PP_THREADS" \
      --dram_buffer_pool_gib="$DRAM_BP_BASELINE" \
      --dram_recordcache_gib=0.0 \
      2>&1 | tee "$out" || exit_code=$?

   local elapsed=$(( $(date +%s) - start_ts ))
   if [[ "$exit_code" -eq 0 ]]; then mark_pass "bf-tree TPC-C (${elapsed}s)"
   else                              mark_fail "bf-tree TPC-C (exit=${exit_code}, ${elapsed}s)"
   fi
}

# -----------------------------------------------------------------------------
log_phase "5/25 retry of pending failures (post-D7 adapter fix + bf payload_length fix)"
log_info "result_dir = $RESULT_DIR"
log_info "PREREQ: cd build && cmake --build . -j  (LeanStoreAdapter.hpp was edited)"

OVERALL_START=$(date +%s)
run_bf_e
run_bf_f
run_ht_d
run_tl_tpcc
run_bf_a
run_bf_tpcc
OVERALL_SEC=$(( $(date +%s) - OVERALL_START ))
OVERALL_MIN=$(awk "BEGIN {printf \"%.1f\", $OVERALL_SEC / 60.0}")

log_phase "RETRY DONE"
log_info "total elapsed = ${OVERALL_SEC}s (${OVERALL_MIN} min)"
log_info "pass=${PASS_COUNT} fail=${FAIL_COUNT} / 6"

if [[ ${#FAILED_TESTS[@]} -gt 0 ]]; then
   echo ""
   echo "STILL FAILING:"
   for t in "${FAILED_TESTS[@]}"; do echo "  - $t"; done
fi

echo ""
echo "Results in: $RESULT_DIR"
ls -1 "$RESULT_DIR"
