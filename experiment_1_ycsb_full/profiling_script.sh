TIMESTAMP=$(date +%Y%m%d_%H%M%S) \
&& SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)" \
&& REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)" \
&& RESULT_DIR="$SCRIPT_DIR/result_${TIMESTAMP}_smaller_dram_bp" \
&& mkdir -p "$RESULT_DIR" \
&& WORKING_SET_GIB=4.0 \
&& PAYLOAD_SIZE_BYTES=100 \
&& DRAM_BP_TWO_LEVEL=0.125 \
&& DRAM_RC_TWO_LEVEL=0.5 \
&& CXL_GIB=3.0 \
&& WORKER_THREADS=4 \
&& PP_THREADS=1 \
&& CXL_PP_THREADS=1 \
&& TWO_LEVEL_ADMISSION_THREADS=2 \
&& FORWARD_EPOCH_THREAD=1 \
&& SIEVE_EVICTION_THREAD=1 \
&& RECORD_CACHE_PROMOTE_THREAD=4 \
&& WARMUP_LOOKUPS=30000000 \
&& MEASURE_LOOKUPS=10000000 \
&& PROGRESS_INTERVAL=1000000 \
&& WARMUP_PROGRESS_INTERVAL=2000000 \
&& SSD_PATH="/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_comparison" \
&& CXL_DAX_DEVICE="/dev/dax0.2" \
&& BUILD_DIR="$REPO_ROOT/build/frontend" \
&& echo "RESULT_DIR=$RESULT_DIR" \
&& for theta in 0.90 0.95 0.99; do
     for wl in b c; do
       binary="$BUILD_DIR/experiment_1_ycsb_${wl}"
       result_file="$RESULT_DIR/result_ycsb${wl}_two_level_theta${theta}_ws${WORKING_SET_GIB}gib_${TIMESTAMP}.log"

       theta_flags=()
       case "$theta" in
         0.90)
           theta_flags+=(
             --skew_threshold_ratio=0.08
             --uniform_threshold_ratio=0.45
             --max_per_page_visits=8000
             --max_global_requests_window=2000000
             --admission_aging_interval=80000
             --trigger_visit_histogram_update_size=160000
           )
           ;;
         0.95)
           theta_flags+=(
             --skew_threshold_ratio=0.10
             --uniform_threshold_ratio=0.55
             --max_per_page_visits=6000
             --max_global_requests_window=1500000
             --admission_aging_interval=60000
             --trigger_visit_histogram_update_size=120000
           )
           ;;
         0.99)
           theta_flags+=(
             --skew_threshold_ratio=0.12
             --uniform_threshold_ratio=0.65
             --max_per_page_visits=4000
             --max_global_requests_window=1000000
             --admission_aging_interval=40000
             --trigger_visit_histogram_update_size=80000
           )
           ;;
       esac

       echo "[RUN] wl=$wl theta=$theta -> $result_file"
       exit_code=0

       "$binary" \
         --test_admission_mode=two_level \
         --test_zipf_theta="$theta" \
         --cxl_tiering_enabled=true \
         --cxl_gib="$CXL_GIB" \
         --cxl_dax_device_path="$CXL_DAX_DEVICE" \
         --pp_threads="$PP_THREADS" \
         --cxl_pp_threads="$CXL_PP_THREADS" \
         --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS" \
         --delay_admission_recordcache_threads_start=true \
         --dram_buffer_pool_gib="$DRAM_BP_TWO_LEVEL" \
         --dram_recordcache_gib="$DRAM_RC_TWO_LEVEL" \
         --forward_epoch_thread="$FORWARD_EPOCH_THREAD" \
         --sieve_eviction_thread="$SIEVE_EVICTION_THREAD" \
         --record_cache_promote_thread="$RECORD_CACHE_PROMOTE_THREAD" \
         "${theta_flags[@]}" \
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

       if [ "$exit_code" -ne 0 ]; then
         echo "[WARN] wl=$wl theta=$theta exit code $exit_code (continuing)"
       fi

       echo "[INFO] cooldown 30s"
       sleep 30

     done
   done \
&& echo "[DONE] all runs complete in $RESULT_DIR"