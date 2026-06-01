add_executable(frontend frontend.cpp)
add_dependencies(frontend leanstore)
target_link_libraries(frontend leanstore)

add_executable(bulk_loading mix/bulk_loading.cpp)
target_link_libraries(bulk_loading leanstore Threads::Threads)
target_include_directories(bulk_loading PRIVATE ${SHARED_INCLUDE_DIRECTORY})

add_executable(ycsb ycsb/ycsb.cpp)
target_link_libraries(ycsb leanstore Threads::Threads)
target_include_directories(ycsb PRIVATE ${SHARED_INCLUDE_DIRECTORY})

add_executable(deterministic ycsb/deterministic.cpp)
target_link_libraries(deterministic leanstore Threads::Threads)
target_include_directories(deterministic PRIVATE ${SHARED_INCLUDE_DIRECTORY})

add_executable(tpcc tpc-c/tpcc.cpp)
target_link_libraries(tpcc leanstore Threads::Threads)
target_include_directories(tpcc PRIVATE ${SHARED_INCLUDE_DIRECTORY})

add_executable(graveyard micro-benchmarks/graveyard.cpp)
target_link_libraries(graveyard leanstore Threads::Threads)
target_include_directories(graveyard PRIVATE ${SHARED_INCLUDE_DIRECTORY})

add_executable(queue micro-benchmarks/queue.cpp)
target_link_libraries(queue leanstore Threads::Threads)
target_include_directories(queue PRIVATE ${SHARED_INCLUDE_DIRECTORY})

add_executable(minimal_example minimal-example/main.cpp)
target_link_libraries(minimal_example leanstore Threads::Threads)
target_include_directories(minimal_example PRIVATE ${SHARED_INCLUDE_DIRECTORY})

add_executable(scaled_example scaled-example/main.cpp)
target_link_libraries(scaled_example leanstore Threads::Threads)
target_include_directories(scaled_example PRIVATE ${SHARED_INCLUDE_DIRECTORY})

add_executable(rocksdb_tpcc tpc-c/rocksdb_tpcc.cpp shared/rocksdb.cpp)
target_link_libraries(rocksdb_tpcc leanstore rocksdb Threads::Threads dl z libbz2.a lz4 snappy zstd uring)
target_include_directories(rocksdb_tpcc PRIVATE ${SHARED_INCLUDE_DIRECTORY})
target_compile_definitions(rocksdb_tpcc PUBLIC ROCKSDB_ADAPTER)

add_executable(rocksdb_ycsb ycsb/rocksdb_ycsb.cpp shared/rocksdb.cpp)
target_link_libraries(rocksdb_ycsb leanstore rocksdb Threads::Threads dl z libbz2.a lz4 snappy zstd uring)
target_include_directories(rocksdb_ycsb PRIVATE ${SHARED_INCLUDE_DIRECTORY})
target_compile_definitions(rocksdb_ycsb PUBLIC ROCKSDB_ADAPTER)
# sudo apt-get install libsnappy-dev  zlib1g-dev libbz2-dev liblz4-dev libzstd-dev librocksdb-dev

add_executable(wiredtiger_tpcc tpc-c/wiredtiger_tpcc.cpp)
target_link_libraries(wiredtiger_tpcc leanstore wiredtiger Threads::Threads dl z libbz2.a lz4 snappy)
target_include_directories(wiredtiger_tpcc PRIVATE ${SHARED_INCLUDE_DIRECTORY})

add_executable(wiredtiger_ycsb ycsb/wiredtiger_ycsb.cpp)
target_link_libraries(wiredtiger_ycsb leanstore wiredtiger Threads::Threads dl z libbz2.a lz4 snappy)
target_include_directories(wiredtiger_ycsb PRIVATE ${SHARED_INCLUDE_DIRECTORY})
# prefer https://source.wiredtiger.com/10.0.0/build-posix.html over sudo apt-get install wiredtiger libwiredtiger-dev


add_executable(lmdb_tpcc tpc-c/lmdb_tpcc.cpp)
target_link_libraries(lmdb_tpcc leanstore lmdb Threads::Threads)
target_include_directories(lmdb_tpcc PRIVATE ${SHARED_INCLUDE_DIRECTORY})

add_executable(lmdb_ycsb ycsb/lmdb_ycsb.cpp)
target_link_libraries(lmdb_ycsb leanstore lmdb Threads::Threads)
target_include_directories(lmdb_ycsb PRIVATE ${SHARED_INCLUDE_DIRECTORY})
# sudo apt-get install liblmdb-dev

add_executable(cxl_lookup_test ../record-cache-test-suit/test-cxl-lookup-integration.cpp)
target_link_libraries(cxl_lookup_test leanstore Threads::Threads)
target_include_directories(cxl_lookup_test PRIVATE ${SHARED_INCLUDE_DIRECTORY})

# cxl_update_test validates the Read-Only invalidation state-machine
# (000 -> 011 -> 100). Under Write-Through these transitions no longer occur:
# updates SeqLock-sync into the entry instead of logically deleting it. The
# test must be rewritten for WT semantics before it can be re-enabled.
# add_executable(cxl_update_test ../record-cache-test-suit/test-cxl-update-integration.cpp)
# target_link_libraries(cxl_update_test leanstore Threads::Threads)
# target_include_directories(cxl_update_test PRIVATE ${SHARED_INCLUDE_DIRECTORY})

add_executable(cxl_lookup_update_test ../record-cache-test-suit/test-cxl-lookup-update-integration.cpp)
target_link_libraries(cxl_lookup_update_test leanstore Threads::Threads)
target_include_directories(cxl_lookup_update_test PRIVATE ${SHARED_INCLUDE_DIRECTORY})

add_executable(cxl_lookup_pure_record_skew_test ../correctness-and-parameter-validation/Experiment1_pure_skew/test-cxl-lookup-pure-record-skew.cpp)
target_link_libraries(cxl_lookup_pure_record_skew_test leanstore Threads::Threads)
target_include_directories(cxl_lookup_pure_record_skew_test PRIVATE ${SHARED_INCLUDE_DIRECTORY})

add_executable(cxl_lookup_pure_without_recordcache_test ../correctness-and-parameter-validation/Experiment1_pure_skew/test-cxl-lookup-pure-without-recordcache.cpp)
target_link_libraries(cxl_lookup_pure_without_recordcache_test leanstore Threads::Threads)
target_include_directories(cxl_lookup_pure_without_recordcache_test PRIVATE ${SHARED_INCLUDE_DIRECTORY})

add_executable(ycsb_c_test ../ycsb_ablation_study/workload_c/ycsb_c_test.cpp)
target_link_libraries(ycsb_c_test leanstore Threads::Threads)
target_include_directories(ycsb_c_test PRIVATE ${SHARED_INCLUDE_DIRECTORY})

add_executable(ycsb_c_compatile_test ../ycsb_ablation_study/workload_c/ycsb_c_compatile_test.cpp)
target_link_libraries(ycsb_c_compatile_test leanstore Threads::Threads)
target_include_directories(ycsb_c_compatile_test PRIVATE ${SHARED_INCLUDE_DIRECTORY})

# experiment_1_ycsb_full workloads (A-F)
set(EXPERIMENT1_YCSB_WORKLOADS a b c d e f)
foreach(wl IN LISTS EXPERIMENT1_YCSB_WORKLOADS)
  add_executable(experiment_1_ycsb_${wl}
    ../experiment_1_ycsb_full/experiment1_ycsb_${wl}.cpp
  )
  target_link_libraries(experiment_1_ycsb_${wl} leanstore Threads::Threads)
  target_include_directories(experiment_1_ycsb_${wl} PRIVATE ${SHARED_INCLUDE_DIRECTORY})
endforeach()

# TPC-C comparison test binary
add_executable(tpcc_compare_test
  ../experiment_1_ycsb_full/tpcc_compare_test.cpp
)
target_link_libraries(tpcc_compare_test leanstore Threads::Threads)
target_include_directories(tpcc_compare_test PRIVATE ${SHARED_INCLUDE_DIRECTORY})