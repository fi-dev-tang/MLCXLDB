#include "gflags/gflags.h"
// -------------------------------------------------------------------------------------
DEFINE_double(dram_gib, 32, "");
DEFINE_double(ssd_gib, 1700, "");
DEFINE_uint32(free_pct, 1, "pct");
DEFINE_uint32(dram_partition_bits, 6, "bits per partition");     // partition to 64 shards
DEFINE_uint32(pp_threads, 1, "number of page provider threads");
DEFINE_bool(worker_page_eviction, false, "");
// -------------------------------------------------------------------------------------
DEFINE_string(csv_path, "./log", "");
DEFINE_bool(csv_truncate, false, "");
DEFINE_string(ssd_path, "./leanstore", "Position of SSD, gets persisted");
DEFINE_uint32(write_buffer_size, 1024, "");
DEFINE_bool(trunc, false, "Truncate file");
DEFINE_uint32(falloc, 0, "Preallocate GiB");
// -------------------------------------------------------------------------------------
DEFINE_bool(print_debug, true, "");
DEFINE_bool(print_tx_console, true, "");
DEFINE_uint32(print_debug_interval_s, 1, "");
DEFINE_bool(profiling, false, "");
DEFINE_bool(profile_latency, false, "");
DEFINE_bool(crc_check, false, "");
// -------------------------------------------------------------------------------------
DEFINE_uint32(worker_threads, 4, "");
DEFINE_bool(cpu_counters, true, "Disable if HW does not have enough counters for all threads");
DEFINE_bool(pin_threads, false, "Responsibility of the driver");
DEFINE_bool(smt, true, "Simultaneous multithreading");
// -------------------------------------------------------------------------------------
DEFINE_bool(root, false, "does this process have root rights ?");
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
DEFINE_string(zipf_path, "/bulk/zipf", "");
DEFINE_double(zipf_factor, 0.0, "");
DEFINE_double(target_gib, 0.0, "size of dataset in gib (exact interpretation depends on the driver)");
DEFINE_uint64(run_for_seconds, 10, "Keep the experiment running for x seconds");
DEFINE_uint64(warmup_for_seconds, 10, "Warmup for x seconds");
// -------------------------------------------------------------------------------------
DEFINE_bool(contention_split, true, "");
DEFINE_uint64(cm_update_on, 7, "as exponent of 2");
DEFINE_uint64(cm_period, 14, "as exponent of 2");
DEFINE_uint64(cm_slowpath_threshold, 1, "");
// -------------------------------------------------------------------------------------
DEFINE_bool(xmerge, false, "");
DEFINE_uint64(xmerge_k, 5, "");
DEFINE_double(xmerge_target_pct, 80, "");
// -------------------------------------------------------------------------------------
DEFINE_bool(optimistic_scan, true, "Jump to next leaf directly if the pointer in the parent has not changed");
DEFINE_bool(measure_time, false, "");
// -------------------------------------------------------------------------------------
DEFINE_double(tmp1, 0.0, "for ad-hoc experiments");
DEFINE_double(tmp2, 0.0, "");
DEFINE_double(tmp3, 0.0, "");
DEFINE_double(tmp4, 0.0, "");
DEFINE_double(tmp5, 0.0, "");
DEFINE_double(tmp6, 0.0, "");
DEFINE_double(tmp7, 0.0, "");
// -------------------------------------------------------------------------------------
DEFINE_bool(btree_print_height, false, "Print BTree height in destructor");
DEFINE_bool(btree_print_tuples_count, false, "Print # tuples in each BTree in destructor");
DEFINE_bool(btree_prefix_compression, true, "");
DEFINE_bool(btree_heads, true, "Enable heads optimization in lowerBound search");
DEFINE_int64(btree_hints, 1, "0: disabled 1: serial 1: AVX512");
DEFINE_bool(nc_reallocation, false, "Reallocate hot pages in non-clustered btree index");
// -------------------------------------------------------------------------------------
DEFINE_bool(bulk_insert, false, "");
// -------------------------------------------------------------------------------------
DEFINE_int64(trace_dt_id, -1, "Print a stack trace for page reads for this DT ID");
DEFINE_int64(trace_trigger_probability, 100, "");
DEFINE_bool(pid_tracing, false, "");
// -------------------------------------------------------------------------------------
DEFINE_string(tag, "", "Unique identifier for this, will be appended to each line csv");
// -------------------------------------------------------------------------------------
DEFINE_bool(optimistic_parent_pointer, false, "");
DEFINE_bool(out_of_place, false, "Out of place writes");
DEFINE_uint64(replacement_chunk_size, 64, "Replacement strategy chunk size");
DEFINE_bool(recycle_pages, true, "");
// -------------------------------------------------------------------------------------
DEFINE_bool(wal, true, "");
DEFINE_bool(wal_rfa, true, "Remote Flush Avoidance (RFA)");
DEFINE_bool(wal_tuple_rfa, true, "tuple-wise tracking");
DEFINE_uint64(wal_offset_gib, 10, "");
DEFINE_bool(wal_pwrite, false, "Does not really write logs on SSD");
DEFINE_bool(wal_fsync, false, "");
DEFINE_int64(wal_variant, 0, "");
DEFINE_uint64(wal_log_writers, 1, "");
DEFINE_uint64(wal_buffer_size, 1024 * 1024 * 10, "");
// -------------------------------------------------------------------------------------
DEFINE_string(isolation_level, "si", "options: ru (READ_UNCOMMITTED), rc (READ_COMMITTED), si (SNAPSHOT_ISOLATION), ser (SERIALIZABLE)");
DEFINE_bool(mv, true, "Multi-version");
DEFINE_uint64(si_refresh_rate, 0, "");
DEFINE_bool(todo, true, "");
// -------------------------------------------------------------------------------------
DEFINE_bool(vi, true, "BTree with SI using in-place version");
DEFINE_bool(vi_delta, true, "");
DEFINE_bool(vi_utodo, true, "");
DEFINE_bool(vi_rtodo, true, "");
DEFINE_bool(vi_flookup, false, "");
DEFINE_bool(vi_fremove, false, "");
DEFINE_bool(vi_update_version_elision, false, "");
DEFINE_bool(vi_fupdate_chained, false, "");
DEFINE_bool(vi_fupdate_fat_tuple, false, "");
DEFINE_uint64(vi_pgc_batch_size, 2, "");
DEFINE_bool(vi_fat_tuple, false, "");
DEFINE_string(vi_fat_tuple_dts, "", "");
DEFINE_bool(vi_fat_tuple_decompose, true, "");
DEFINE_uint64(vi_fat_tuple_trigger, 0, "1: oldest_oltp, 1: probability");
DEFINE_bool(vi_fat_tuple_alternative, false, "hit the previous version at every update");
DEFINE_bool(vi_dangling_pointer, true, "");
// -------------------------------------------------------------------------------------
DEFINE_bool(olap_mode, true, "Use OLAP mode for long running transactions");
DEFINE_bool(graveyard, true, "Use Graveyard Index");
// -------------------------------------------------------------------------------------
DEFINE_bool(pgc, true, "Precise garbage collection/recycling");
DEFINE_uint64(pgc_variant, 0, "0 naive, 1 bit faster, 2 ...");
DEFINE_double(garbage_in_page_pct, 15, "Threshold to trigger page-wise garbage collection (%)");
DEFINE_uint64(vi_max_chain_length, 1000, "");
DEFINE_uint64(todo_batch_size, 1024, "");
DEFINE_bool(history_tree_inserts, true, "");
// -------------------------------------------------------------------------------------
DEFINE_bool(persist, false, "");
DEFINE_bool(recover, false, "");
DEFINE_string(persist_file, "./leanstore.json", "Where should the persist config be saved to?");
DEFINE_string(recover_file, "./leanstore.json", "Where should the recover config be loaded from?");
//--------------------------------[Added]. cxl allocation related------------------------
DEFINE_bool(cxl_tiering_enabled, false, "[CXL] Enable Storage from DRAM-SSD to DRAM-CXL-SSD");
DEFINE_bool(enable_record_cache, true, "[CXL] Enable DRAM RecordCache path");
// [v4-a] Skip the 2B dt_id prefix on RC keys. Safe ONLY for single-BTreeVI-table
// workloads (e.g. YCSB). MUST be false for TPC-C (multiple tables share folded-key
// shape and would collide in the byte-keyed hash). Process-wide setting; flipping
// at runtime would orphan existing entries.
DEFINE_bool(rc_skip_dt_id_prefix, false,
            "[CXL][RC] Skip the 2B dt_id prefix on RecordCache keys. Single-table only "
            "(YCSB=true, TPC-C=false). Saves a stack memcpy + 2B in the key hashed.");
// [v4-a] Upper bound on optimistic SeqLock reread attempts in tryLookupInRecordCache.
// Lower = give up to CXL path sooner under writer contention (cheaper miss);
// higher = squeeze every retry before falling back. 8 was the original hard-coded.
DEFINE_uint32(rc_seqlock_retry_max, 8,
              "[CXL][RC] Max SeqLock retry count in optimistic RC read before giving up "
              "to CXL fallback. Range typically 2-16.");
DEFINE_double(cxl_gib, 128, "[CXL] CXL Buffer Pool size in GiB");
DEFINE_double(dram_buffer_pool_gib, 16, "[CXL] DRAM Buffer Pool size in GiB");
DEFINE_double(dram_recordcache_gib, 16, "[CXL] DRAM RecordCache size in GiB");
// devdax devices path
DEFINE_string(cxl_dax_device_path, "/dev/dax0.0", "CXL dax device path for mmap");
DEFINE_uint32(cxl_partition_bits, 7, "cxl bits per partition");     // partition to 128 shards, tunable
DEFINE_uint32(cxl_pp_threads, 2, "[CXL] number of CXL page provider threads (Evict Cold CXL Pages to SSD)");
DEFINE_uint32(two_level_admission_threads, 2, "number of two level admission control threads");
DEFINE_uint32(forward_epoch_thread, 1, "[CXL] Invalidation thread specific for RecordCacheEntry invalidation");
DEFINE_uint32(sieve_eviction_thread, 1, "[CXL] SIEVE Eviction thread specific for RecordCacheEntry eviction");
DEFINE_uint32(record_cache_promote_thread, 1, "[CXL] RecordCache Promote Thread");
DEFINE_bool(delay_admission_recordcache_threads_start, false,
            "[CXL] Delay TwoLevelAdmission + RecordCache background threads start until explicitly enabled");
DEFINE_string(admission_mode, "two_level",
              "[CXL] Admission mode: two_level | page_only | lru | bf-tree | hybried-tier-asplos2025 | tiered-indexing-zxj");
DEFINE_uint64(ti_lazy_migration_pct, 100,
              "[CXL][Tiered-Indexing] Upward migration sampling probability (0-100). 100 = eager (always migrate)");
DEFINE_bool(admission_scan_mode, false,
            "[CXL] Scan-workload mode: skip Condition-0, prioritize promoteFullPage, epoch-timeout promotes");
DEFINE_uint64(lru_background_promote_batch, 64,
              "[CXL][LRU admission] max pages promoted per background cycle");
// ----------[Added]. Two-Level Admission Control tuning parameters -----------------
// (1) Page-Level CountMinSketch (PageCountMinSketch)
DEFINE_uint64(page_cms_row_num, 12,
              "[CXL][AdmCtrl] Number of hash functions for page-level CountMinSketch");
DEFINE_uint64(page_cms_col_num, 1024 * 1024,
              "[CXL][AdmCtrl] Floor for page-level CMS column width. "
              "Actual width is auto-derived in the Wrapper ctor from "
              "(cxl_gib + dram_bp + dram_rc) and clamped to [this, 8M].");
// (2) Record-Level CountMinSketch (RecordCountMinSketch)
DEFINE_uint64(record_cms_row_num, 4,
              "[CXL][AdmCtrl] Number of hash functions for record-level CountMinSketch");
DEFINE_uint64(record_cms_col_num, 1024 * 64,
              "[CXL][AdmCtrl] Column width for record-level CountMinSketch");
// (3) Visit Frequency Histogram (VisitFrequencyBucketArray)
DEFINE_uint64(visit_histogram_bucket_num, 16,
              "[CXL][AdmCtrl] Number of buckets in the sampled visit frequency histogram");
// (4) Duplicate-Set for Page Sampling (DepulicateSetForPageSampling)
DEFINE_uint64(max_sampled_page_ids, 4096,
              "[CXL][AdmCtrl] Max distinct page IDs kept in the sampling set per histogram update window");
// (5) DramHotPageCandidates admission / eviction parameters
DEFINE_double(skew_threshold_ratio, 0.10,
              "[CXL][AdmCtrl] Fraction of per-page visits a single slot must exceed to be considered skewed");
DEFINE_double(uniform_threshold_ratio, 0.50,
              "[CXL][AdmCtrl] Fraction of slots that must be accessed before a page is promoted as uniform");
DEFINE_uint64(max_per_page_visits, 3000,
              "[CXL][AdmCtrl] Per-page visit count window for skew/uniform decision");
DEFINE_uint64(max_global_requests_window, 500000,
              "[CXL][AdmCtrl] Global request window after which a stale HotPageCandidate is evicted");
DEFINE_uint64(trigger_visit_histogram_update_size, 1024 * 1024,
              "[CXL][AdmCtrl] Number of page accesses that triggers both histogram rebuild and RecordCMS aging");