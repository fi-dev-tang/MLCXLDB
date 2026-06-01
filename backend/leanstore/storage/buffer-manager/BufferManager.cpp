#include "BufferManager.hpp"

#include "AsyncWriteBuffer.hpp"
#include "BufferFrame.hpp"
#include "Exceptions.hpp"
#include "leanstore/Config.hpp"
#include "leanstore/profiling/counters/CPUCounters.hpp"
#include "leanstore/profiling/counters/PPCounters.hpp"
#include "leanstore/profiling/counters/WorkerCounters.hpp"
#include "leanstore/utils/FVector.hpp"
#include "leanstore/utils/Misc.hpp"
#include "leanstore/utils/Parallelize.hpp"
#include "leanstore/utils/RandomGenerator.hpp"
// -------------------------------------------------------------------------------------
#include <gflags/gflags.h>
// -------------------------------------------------------------------------------------
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <set>
// -------------------------------------------------------------------------------------
namespace leanstore
{
namespace storage
{
// -------------------------------------------------------------------------------------
thread_local BufferFrame* BufferManager::last_read_bf = nullptr;
// -------------------------------------------------------------------------------------

// ------------------------------[Added].-----------------------------------------------
// Changing Initialization logic for CXL-Tiered Memory
// bfs --> [dram_buffer_pool]                ====> BufferFrame * free_list Partition
// record_cache_ptr --> [dram_record_cache]  ====> RecordCacheEntry (Slab Allocator)
// cxl_bfs --> [cxl_buffer_pool]             ====> BufferFrame * free_list Partition
// Rewrite BufferManager::BufferManager: mmap twice for dram_buffer_pool and dram_record_cache, mmap dax devices for cxl_buffer_pool
BufferManager::BufferManager(s32 ssd_fd): ssd_fd(ssd_fd)
{
   // -------------------------------------------------------------------------------------
   // Init the following three parts:
   // 1. DRAM Buffer Pool 
   // 2. CXL Buffer Pool(if cxl_tiering_enabled)
   // 3. DRAM RecordCache(if cxl_tiering_enabled)
   // Global parameters
   // Using *_in_safety_bytes to represent additional safety pages or safety record_cache_entries
   // Using *_num to exclude additional safety boundaries
   // When CXL tiering is OFF, use --dram_buffer_pool_gib directly as the total
   // DRAM budget (no CXL, no RecordCache DRAM, so the entire DRAM goes to the
   // buffer pool).  This avoids requiring callers to pass --dram_gib separately.
   // When CXL tiering is ON, --dram_buffer_pool_gib controls only the DRAM buffer
   // pool slice; --dram_recordcache_gib controls the RecordCache slice separately.
   if (!FLAGS_cxl_tiering_enabled) {
      // When CXL tiering is OFF, the entire DRAM budget goes to the buffer pool.
      // Sync FLAGS_dram_gib from FLAGS_dram_buffer_pool_gib so that downstream
      // code that reads FLAGS_dram_gib (e.g. WiredTiger, RocksDB adapters) sees
      // the correct value.  Callers do NOT need to pass --dram_gib separately.
      FLAGS_dram_gib = FLAGS_dram_buffer_pool_gib;
   } else {
      if (FLAGS_enable_record_cache && FLAGS_dram_recordcache_gib > 0.0) {
         FLAGS_dram_gib = FLAGS_dram_buffer_pool_gib + FLAGS_dram_recordcache_gib;
      } else {
         FLAGS_dram_gib = FLAGS_dram_buffer_pool_gib;
      }
   }

   dram_buffer_pool_frame_num = FLAGS_dram_buffer_pool_gib * 1024ULL * 1024 * 1024 / sizeof(BufferFrame);
   const u64 dram_buffer_pool_in_safety_bytes = sizeof(BufferFrame) * (dram_buffer_pool_frame_num + safety_pages);

   const double effective_dram_pool_gib = FLAGS_dram_buffer_pool_gib;   
   u64 cxl_total_bytes = 0;
   u64 dram_record_cache_in_safety_bytes = 0;
   partitions_count = (1 << FLAGS_dram_partition_bits);

   const bool enable_record_cache_runtime = FLAGS_cxl_tiering_enabled &&
                                            FLAGS_enable_record_cache &&
                                            FLAGS_dram_recordcache_gib > 0.0;

   if(FLAGS_cxl_tiering_enabled){
      cxl_buffer_pool_frame_num = FLAGS_cxl_gib * 1024ULL * 1024 * 1024 / sizeof(BufferFrame);

      if (enable_record_cache_runtime) {
         // [Added]: Allocate the full RecordCache DRAM budget as a contiguous byte region.
         //
         // IMPORTANT: dram_record_cache_entries_num cannot be computed accurately here because
         // BufferManager does not know the application-level key/value sizes.  Each
         // RecordCacheEntry in DRAM occupies:
         //   sizeof(RecordCacheEntry) [16 B header] + key_length + value_length
         // which varies per workload.  Dividing only by sizeof(RecordCacheEntry) would
         // grossly overestimate the entry count and underestimate memory usage.
         //
         // Instead we treat the entire GiB budget as raw bytes handed to RecordCacheSlabAllocator,
         // which manages internal fragmentation itself.  We add one extra 2 MiB slab as a safety
         // margin (matching RecordCacheSlabAllocator::HUGE_PAGE_SIZE) to avoid boundary faults.
         const u64 rc_budget_bytes  = static_cast<u64>(FLAGS_dram_recordcache_gib * 1024.0 * 1024.0 * 1024.0);
         const u64 rc_safety_bytes  = 2ULL * 1024 * 1024;   // one extra slab = HUGE_PAGE_SIZE
         dram_record_cache_in_safety_bytes = rc_budget_bytes + rc_safety_bytes;
         // dram_record_cache_entries_num is kept as an approximate lower-bound for logging only
         // (assumes minimum entry size = sizeof(RecordCacheEntry) = 16 B; real capacity is lower).
         dram_record_cache_entries_num = rc_budget_bytes / sizeof(RecordCacheEntry);
      } else {
         dram_record_cache_entries_num = 0;
         dram_record_cache_in_safety_bytes = 0;
      }
      // dram_total_bytes = dram_buffer_pool_in_safety_bytes + dram_record_cache_in_safety_bytes;
      
      const u64 cxl_buffer_pool_in_safety_bytes = sizeof(BufferFrame) * (cxl_buffer_pool_frame_num + safety_pages);
      
      // [Added]. according to devdax mmap, it requires 2 MiB aligned.
      constexpr u64 CXL_ALIGNMENT = 2ULL * 1024 * 1024; // 2 MiB
      cxl_total_bytes = (cxl_buffer_pool_in_safety_bytes + CXL_ALIGNMENT - 1) & ~(CXL_ALIGNMENT - 1);

      partitions_count = partitions_count + (1 << FLAGS_cxl_partition_bits);
   }
   
   //================================================================================
   // Part 1: DRAM Buffer Pool Allocation
   // [Caution]: mmap and memset larger page(includes safety additional bytes)
   // [Caution]: partition actual bytes(exclude additional safety boundaries)
   //================================================================================
   {
      void *big_dram_memory_chunk = mmap(NULL, dram_buffer_pool_in_safety_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE| MAP_ANONYMOUS, -1, 0);
      
      if(big_dram_memory_chunk == MAP_FAILED){
         perror("Failed to allocate memory for dram buffer pool");
         SetupFailed("Check the dram_gib size");
      }else{
         bfs = reinterpret_cast<BufferFrame*>(big_dram_memory_chunk);
      }

      madvise(bfs, dram_buffer_pool_in_safety_bytes, MADV_HUGEPAGE);      // Huge page configuration
      madvise(bfs, dram_buffer_pool_in_safety_bytes, MADV_DONTFORK);      // Protect O_Direct's memory align, fork can not inhereit this memory region, bypass os page cache
   }
   //========================================================================================================
   // Part 2: dram buffer pool partition
   // partitions: [dram_buffer_pool_partitions][...]
   // default: 64 shards for dram_buffer_pool 128 shards for cxl_buffer_pool
   //========================================================================================================
   {
      dram_partitions_count = (1 << FLAGS_dram_partition_bits);   // default: 64
      dram_partitions_mask = dram_partitions_count - 1;           // default: 0011 1111

      // Every partition should have at dram_free_bfs_limit's page.
      // 100000 pages, 64 shards, every partition should have 16 free pages.
      const u64 dram_free_bfs_limit = std::ceil(FLAGS_free_pct * 1.0 * dram_buffer_pool_frame_num / 100.0) / static_cast<double>(dram_partitions_count);

      for(u64 p_i = 0; p_i < dram_partitions_count; p_i++){
         partitions.push_back(
            std::make_unique<Partition>(p_i, dram_partitions_count, dram_free_bfs_limit)
         );
      }

      // Initialize Global Two-Level Admission Control if CXL tiering is enabled
      if (FLAGS_cxl_tiering_enabled) {
         global_admission_control = new two_level_admission_control::TwoLevelAdmissionControlWrapper();
      }

      // memset to zero
      // [Caution]: parallelRange only deals with block_size, if call_back is memset(bytes granularity), using bytes
      utils::Parallelize::parallelRange(dram_buffer_pool_in_safety_bytes, [&](u64 begin, u64 end){
         memset(reinterpret_cast<u8*>(bfs) + begin, 0, end - begin);
      });
      
      // round-robin to partitions
      // [Caution]: parallelRange only deals with block_size, if call_back is partitions(BufferFrame granularity), using nums
      utils::Parallelize::parallelRange(dram_buffer_pool_frame_num, [&](u64 bf_b, u64 bf_e){
         for(u64 bf_i = bf_b; bf_i < bf_e; bf_i++){
            u64 p_i = bf_i % dram_partitions_count;
            partitions[p_i] -> dram_free_list.push(*new (bfs + bf_i) BufferFrame()); 
         }
      });
   }
   // -------------[Added]. big_cxl_memory_chunk---------------------------------------------------------------------
   //======================================================
   // Part 3: CXL Memory Allocation(mmap dax devices)
   //======================================================
   if(FLAGS_cxl_tiering_enabled){
      int cxl_fd = open(FLAGS_cxl_dax_device_path.c_str(), O_RDWR); // open() receives const char*, change from std::string to const char*
      if(cxl_fd < 0){
         std::cerr << "Error: Cannot open CXL device " << FLAGS_cxl_dax_device_path << std::endl;
         std::cerr << "Hint: Run 'cxlmgmt alloc' first" << std::endl;
         SetupFailed("Check CXL device path, run 'cxlmgmt alloc --socket=0 --size=128' first");
      }

      void *big_cxl_memory_chunk = mmap(NULL, cxl_total_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, cxl_fd, 0);
      close(cxl_fd);

      if(big_cxl_memory_chunk == MAP_FAILED){
         perror("Failed to allocate cxl memory for cxl buffer pool");
         SetupFailed("Check the cxl_gib size");
      }else{
         cxl_bfs = reinterpret_cast<BufferFrame*>(big_cxl_memory_chunk);
      }
      madvise(cxl_bfs, cxl_total_bytes, MADV_HUGEPAGE);   // Huge page configuration
      madvise(cxl_bfs, cxl_total_bytes, MADV_DONTFORK);   // Protect O_Direct's memory align, fork can not inhereit this memory region, bypass os page cache
   }
   //========================================================================================================
   // Part 4: cxl buffer pool partition
   // partitions: [...][cxl_buffer_pool_partitions]
   // default: ... 128 shards for cxl_buffer_pool
   // [TODO]: may use different free_pct for cxl.(Revise Config.hpp)
   //========================================================================================================
   if(FLAGS_cxl_tiering_enabled){
      cxl_partitions_count = (1 << FLAGS_cxl_partition_bits); // default: 128
      cxl_partitions_mask = cxl_partitions_count - 1;         // default: 0111 1111
      
      const u64 cxl_free_bfs_limit = std::ceil(FLAGS_free_pct * 1.0 * cxl_buffer_pool_frame_num / 100.0) / static_cast<double>(cxl_partitions_count);

      // default: cxl_partition_offset starts from partitions[64]
      u64 cxl_partition_offset = partitions.size();

      // default: Allocation start from partitions[64]
      for(u64 p_i = 0; p_i < cxl_partitions_count; p_i++){
         partitions.push_back(std::make_unique<Partition>(p_i, cxl_partitions_count, cxl_free_bfs_limit));
      }

      // memset to zero
      utils::Parallelize::parallelRange(cxl_total_bytes, [&](u64 begin, u64 end){
         memset(reinterpret_cast<u8*>(cxl_bfs) + begin, 0, end - begin);
      });

      // round-robin to cxl partitions
      utils::Parallelize::parallelRange(cxl_buffer_pool_frame_num, [&](u64 bf_b, u64 bf_e){
         for(u64 bf_i = bf_b; bf_i < bf_e; bf_i++){
            u64 p_i = bf_i % cxl_partitions_count;
            partitions[cxl_partition_offset + p_i] -> cxl_free_list.push(*new(cxl_bfs + bf_i) BufferFrame());
         }
      });
   }
   //=============================================================================================
   // Part 5: DRAM  RecordCache Allocation
   // [Caution]: mmap and memset larger record_cache_entries (includes safety additional bytes)
   // [Caution]: partition actual bytes(exclude additional safety boundaries)
   // This boundary-safe allocation strategy may lead to memory fragmentation.
   //=============================================================================================
   if(enable_record_cache_runtime){
      void *big_dram_record_cache_memory_chunk = mmap(NULL, dram_record_cache_in_safety_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE| MAP_ANONYMOUS, -1, 0);
      
      if(big_dram_record_cache_memory_chunk == MAP_FAILED){
         perror("Failed to allocate memory for dram recordcache");
         SetupFailed("Check the dram_gib size");
      }else{
         record_cache_ptr = reinterpret_cast<RecordCacheEntry*>(big_dram_record_cache_memory_chunk);
      }

      madvise(record_cache_ptr, dram_record_cache_in_safety_bytes, MADV_HUGEPAGE);      // Huge page configuration
      madvise(record_cache_ptr, dram_record_cache_in_safety_bytes, MADV_DONTFORK);      // Protect O_Direct's memory align, fork can not inhereit this memory region, bypass os page cache
   
   //=============================================================================================
   // Part 6: DRAM  RecordCache SlabAllocator
   //=============================================================================================
      record_cache_allocator = new recordcache::RecordCacheSlabAllocator(record_cache_ptr, dram_record_cache_in_safety_bytes);

      global_record_cache = new recordcache::RecordCache(*record_cache_allocator, FLAGS_worker_threads);
      // [default]: Using FLAGS_worker_threads as RecordCache HashTable Shard Numbers
   } else if (FLAGS_cxl_tiering_enabled) {
      fprintf(stdout, "[BufferManager] RecordCache disabled (enable_record_cache=false or dram_recordcache_gib<=0)\n");
   }
  
}
// -------------------------------------------------------------------------------------
// =======================[Added].=================================================================
// Add cxl PageProviderThread for CXL evict.
// Switch logic:
//   CXL disabled → pageProviderThread        (original DRAM → SSD, one-letter untouched)
//   CXL enabled  → dramPageProviderThread    (DRAM cooling + demotion to CXL)
//                 + cxlPageProviderThread     (CXL cooling + eviction to SSD)
void BufferManager::startBackgroundThreads()
{
   if (FLAGS_cxl_tiering_enabled) {
      // ====================================================================================
      // Path A: CXL tiering ON
      // ====================================================================================

      // Part A-1: DRAM Page Provider threads  →  dramPageProviderThread
      // Responsibility: DRAM Cooling (HOT → COOL) + Demotion to CXL
      if (FLAGS_pp_threads) {
         std::vector<std::thread> pp_threads;
         const u64 dram_partitions_per_thread = dram_partitions_count / FLAGS_pp_threads;
         ensure(FLAGS_pp_threads <= dram_partitions_count);
         const u64 dram_extra = dram_partitions_count % FLAGS_pp_threads;

         for (u64 t_i = 0; t_i < FLAGS_pp_threads; t_i++) {
            pp_threads.emplace_back(
                [&, t_i](u64 p_begin, u64 p_end) {
                   if (FLAGS_pin_threads) {
                      utils::pinThisThread(FLAGS_worker_threads + FLAGS_wal + t_i);
                   } else {
                      utils::pinThisThread(FLAGS_wal + t_i);
                   }
                   CPUCounters::registerThread("dram_pp_" + std::to_string(t_i));
                   if (FLAGS_root) {
                      posix_check(setpriority(PRIO_PROCESS, 0, -20) == 0);
                   }
                   dramPageProviderThread(p_begin, p_end);
                },
                t_i * dram_partitions_per_thread,
                ((t_i + 1) * dram_partitions_per_thread) + ((t_i == FLAGS_pp_threads - 1) ? dram_extra : 0));
            bg_threads_counter++;
         }
         for (auto& thread : pp_threads) {
            thread.detach();
         }
      }

      // Part A-2: CXL Page Provider threads  →  cxlPageProviderThread
      // Responsibility: CXL Cooling (HOT_CXL → COOL_CXL) + Eviction to SSD
      // CXL partitions live at indices [dram_partitions_count, partitions_count)
      if (FLAGS_cxl_pp_threads) {
         std::vector<std::thread> cxl_pp_threads;
         const u64 cxl_partitions_per_thread = cxl_partitions_count / FLAGS_cxl_pp_threads;
         ensure(FLAGS_cxl_pp_threads <= cxl_partitions_count);
         const u64 cxl_extra = cxl_partitions_count % FLAGS_cxl_pp_threads;

         for (u64 t_i = 0; t_i < FLAGS_cxl_pp_threads; t_i++) {
            cxl_pp_threads.emplace_back(
                [&, t_i](u64 p_begin, u64 p_end) {
                   const u64 thread_offset = FLAGS_pp_threads + t_i;
                   if (FLAGS_pin_threads) {
                      utils::pinThisThread(FLAGS_worker_threads + FLAGS_wal + thread_offset);
                   } else {
                      utils::pinThisThread(FLAGS_wal + thread_offset);
                   }
                   CPUCounters::registerThread("cxl_pp_" + std::to_string(t_i));
                   if (FLAGS_root) {
                      posix_check(setpriority(PRIO_PROCESS, 0, -20) == 0);
                   }
                   cxlPageProviderThread(p_begin, p_end);
                },
                dram_partitions_count + t_i * cxl_partitions_per_thread,
                dram_partitions_count + ((t_i + 1) * cxl_partitions_per_thread) + ((t_i == FLAGS_cxl_pp_threads - 1) ? cxl_extra : 0));
            bg_threads_counter++;
         }
         for (auto& thread : cxl_pp_threads) {
            thread.detach();
         }
      }

      if (!FLAGS_delay_admission_recordcache_threads_start) {
         startAdmissionAndRecordCacheThreads();
      }
   } else {
      // ====================================================================================
      // Path B: CXL tiering OFF  →  original pageProviderThread (untouched)
      // ====================================================================================
      if (FLAGS_pp_threads) {
         std::vector<std::thread> pp_threads;
         const u64 partitions_per_thread = partitions_count / FLAGS_pp_threads;
         ensure(FLAGS_pp_threads <= partitions_count);
         const u64 extra_partitions_for_last_thread = partitions_count % FLAGS_pp_threads;

         for (u64 t_i = 0; t_i < FLAGS_pp_threads; t_i++) {
            pp_threads.emplace_back(
                [&, t_i](u64 p_begin, u64 p_end) {
                   if (FLAGS_pin_threads) {
                      utils::pinThisThread(FLAGS_worker_threads + FLAGS_wal + t_i);
                   } else {
                      utils::pinThisThread(FLAGS_wal + t_i);
                   }
                   CPUCounters::registerThread("pp_" + std::to_string(t_i));
                   if (FLAGS_root) {
                      posix_check(setpriority(PRIO_PROCESS, 0, -20) == 0);
                   }
                   pageProviderThread(p_begin, p_end);
                },
                t_i * partitions_per_thread,
                ((t_i + 1) * partitions_per_thread) + ((t_i == FLAGS_pp_threads - 1) ? extra_partitions_for_last_thread : 0));
            bg_threads_counter++;
         }
         for (auto& thread : pp_threads) {
            thread.detach();
         }
      }
   }
}
// -------------------------------------------------------------------------------------
void BufferManager::startAdmissionAndRecordCacheThreads()
{
   fprintf(stdout, "[startAdmissionAndRecordCacheThreads] enter\n");

   bool expected = false;
   if (!admission_recordcache_threads_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      fprintf(stdout, "[startAdmissionAndRecordCacheThreads] already started, skip\n");
      return;
   }

   fprintf(stdout, "[startAdmissionAndRecordCacheThreads] CAS success, starting threads\n");

   // Part B-1: Two-Level Admission Control threads
   if (FLAGS_two_level_admission_threads > 0) {
      fprintf(stdout,
              "[startAdmissionAndRecordCacheThreads] two-level-admission-threads = %lu\n",
              FLAGS_two_level_admission_threads); 

      std::vector<std::thread> two_level_admission_threads;
      for (u64 t_i = 0; t_i < FLAGS_two_level_admission_threads; t_i++) {
         fprintf(stdout,
                 "[startAdmissionAndRecordCacheThreads] creating two_level_admission thread %lu\n",
                 t_i);

         two_level_admission_threads.emplace_back(
             [&, t_i]() {
                fprintf(stdout, "[two_level_admission thread %lu] lambda entered\n", t_i);

                const u64 thread_offset = FLAGS_pp_threads + FLAGS_cxl_pp_threads + t_i;

                if (FLAGS_pin_threads) {
                   fprintf(stdout,
                           "[two_level_admission thread %lu] before pinThisThread(worker path), thread_offset=%lu\n",
                           t_i, thread_offset);
                   
                   utils::pinThisThread(FLAGS_worker_threads + FLAGS_wal + thread_offset);

                   fprintf(stdout,
                           "[two_level_admission thread %lu] after pinThisThread(worker path)\n",
                           t_i);
                } else {
                   fprintf(stdout,
                           "[two_level_admission thread %lu] before pinThisThread(non-worker path), thread_offset=%lu\n",
                           t_i, thread_offset);
                   
                   utils::pinThisThread(FLAGS_wal + thread_offset);

                   fprintf(stdout,
                           "[two_level_admission thread %lu] after pinThisThread(non-worker path)\n",
                           t_i);
                }

                fprintf(stdout,
                        "[two_level_admission thread %lu] before CPUCounters::registerThread\n",
                        t_i);
                CPUCounters::registerThread("two_level_admission_" + std::to_string(t_i));

                fprintf(stdout,
                        "[two_level_admission thread %lu] after CPUCounters::registerThread\n",
                        t_i);
                

                if (FLAGS_root) {
                   fprintf(stdout,
                           "[two_level_admission thread %lu] before setpriority\n",
                           t_i);
                   

                   posix_check(setpriority(PRIO_PROCESS, 0, -20) == 0);

                   fprintf(stdout,
                           "[two_level_admission thread %lu] after setpriority\n",
                           t_i);
                   
                }

                fprintf(stdout,
                        "[two_level_admission thread %lu] before twoLevelAdmissionThread()\n",
                        t_i);
                

                twoLevelAdmissionThread();

                fprintf(stdout,
                        "[two_level_admission thread %lu] returned from twoLevelAdmissionThread()\n",
                        t_i);
                
             });

         fprintf(stdout,
                 "[startAdmissionAndRecordCacheThreads] created two_level_admission thread %lu\n",
                 t_i);
         

         bg_threads_counter++;
         fprintf(stdout,
                 "[startAdmissionAndRecordCacheThreads] bg_threads_counter=%lu\n",
                 bg_threads_counter.load());
         
      }

      fprintf(stdout, "[startAdmissionAndRecordCacheThreads] detaching two-level-admission threads\n");
      

      for (auto& thread : two_level_admission_threads) {
         thread.detach();
      }

      fprintf(stdout, "[startAdmissionAndRecordCacheThreads] detached all two-level-admission threads\n");
      
   } else {
      fprintf(stdout, "[startAdmissionAndRecordCacheThreads] no two-level-admission threads to start\n");
      
   }

   // Part B-2: RecordCache background threads (ForwardEpoch + SIEVE Eviction + Promote)
   if (global_record_cache != nullptr) {
      fprintf(stdout, "[startAdmissionAndRecordCacheThreads] global_record_cache != nullptr, before startBackgroundThreads()\n");
      global_record_cache->startBackgroundThreads();
      fprintf(stdout, "[startAdmissionAndRecordCacheThreads] returned from global_record_cache->startBackgroundThreads()\n");
      
   } else {
      fprintf(stdout, "[startAdmissionAndRecordCacheThreads] global_record_cache == nullptr, skip startBackgroundThreads()\n");
      
   }
   fprintf(stdout, "[startAdmissionAndRecordCacheThreads] exit\n");
}

// -------------------------------------------------------------------------------------
void BufferManager::enableAdmissionAndRecordCacheThreads()
{
   fprintf(stdout, "[enableAdmissionAndRecordCacheThreads] enter, cxl_tiering_enabled=%s\n" , FLAGS_cxl_tiering_enabled ? "true" : "false");
   if (!FLAGS_cxl_tiering_enabled) {
      fprintf(stdout, "[enableAdmissionAndRecordCacheThreads] Skip: cxl_tiering_disabled\n");
      return;
   }
   fprintf(stdout, "[enableAdmissionAndRecordCacheThreads] Calling startAdmissionAndRecordCacheThreads()\n");
   startAdmissionAndRecordCacheThreads();
}
// -------------------------------------------------------------------------------------------------------
// =======================================================================================================
// PID allocation and reallocation only happen in DRAM, we do not change the logic here!
// =======================================================================================================
// Restore Logic Revisied: 
// store the maximum page_id allocated from all partitions for example, 
// partitions[0]: 25, partitions[1]: 40
// paritions[2]: 58, partitions[3]: 31
// serialize page_id to 58,
// Once restart, allocate start from maximum_page_id allocated(58 here), 
// avoid overwrite already allocated previous dirty pages.
//
// [Caution]: deserialize: max_id must be initialized to a multiple of partitions_count.  
// if partitions_count == 4, then 58 --> 60
std::unordered_map<std::string, std::string> BufferManager::serialize()
{
   std::unordered_map<std::string, std::string> map;
   PID max_pid = 0;
   for (u64 p_i = 0; p_i < dram_partitions_count; p_i++) {
      max_pid = std::max<PID>(getPageID_AllocatorPartition(p_i).next_pid, max_pid);
   }
   map["max_pid"] = std::to_string(max_pid);
   return map;
}
// -------------------------------------------------------------------------------------
void BufferManager::deserialize(std::unordered_map<std::string, std::string> map)
{
   PID max_pid = std::stol(map["max_pid"]);
   max_pid = (max_pid + (dram_partitions_count - 1)) & ~(dram_partitions_count - 1);
   for (u64 p_i = 0; p_i < dram_partitions_count; p_i++) {
      getPageID_AllocatorPartition(p_i).next_pid = max_pid + p_i;
   }
   
}
// -------------------------------------------------------------------------------------
//======================[Added].========================================================
// Make Sure that all dirty pages, whether in dram tier or cxl tier,
// Should all be flushed back to SSD.
void BufferManager::writeAllBufferFrames()
{
   stopBackgroundThreads();
   ensure(!FLAGS_out_of_place);
   // -------------------------------------------------------------------------------------
   // Flush helper: checkpoint and write a single non-free BufferFrame to SSD
   // We derive it to a lambda expression.
   auto flushBf = [&](BufferFrame& bf) {
      BufferFrame::Page page;
      bf.header.latch.mutex.lock();
      if (!bf.isFree()) {
         page.dt_id = bf.page.dt_id;
         page.magic_debugging_number = bf.header.pid;
         DTRegistry::global_dt_registry.checkpoint(bf.page.dt_id, bf, page.dt);
         s64 ret = pwrite(ssd_fd, page, PAGE_SIZE, bf.header.pid * PAGE_SIZE);
         ensure(ret == PAGE_SIZE);
      }
      bf.header.latch.mutex.unlock();
   };
   // -------------------------------------------------------------------------------------
   // Part 1: Flush all DRAM buffer frames
   utils::Parallelize::parallelRange(dram_buffer_pool_frame_num, [&](u64 bf_b, u64 bf_e) {
      for (u64 bf_i = bf_b; bf_i < bf_e; bf_i++) {
         flushBf(bfs[bf_i]);
      }
   });
   // -------------------------------------------------------------------------------------
   // Part 2: Flush all CXL buffer frames (if cxl tiering is enabled)
   if (FLAGS_cxl_tiering_enabled && cxl_bfs != nullptr) {
      utils::Parallelize::parallelRange(cxl_buffer_pool_frame_num, [&](u64 bf_b, u64 bf_e) {
         for (u64 bf_i = bf_b; bf_i < bf_e; bf_i++) {
            flushBf(cxl_bfs[bf_i]);
         }
      });
   }
}
// ------------------------------------------------------------------------------------------------
// Change Name, 
// inUsedPageIDCount: tracks the number of page_ids that have been allocated but not yet reclaimed.
u64 BufferManager::inUsedPageIDCount()
{
   u64 total_allocated = 0, total_freed = 0;
   for (u64 p_i = 0; p_i < dram_partitions_count; p_i++) {
      total_allocated += getPageID_AllocatorPartition(p_i).allocatedPages();
      total_freed += getPageID_AllocatorPartition(p_i).freedPageIDs();
   }
   return total_allocated - total_freed;
}
// -------------------------------------------------------------------------------------
// Buffer Frames Management
// -------------------------------------------------------------------------------------
Partition& BufferManager::randomPartition()
{
   auto rand_partition_i = utils::RandomGenerator::getRand<u64>(0, dram_partitions_count);
   assert(rand_partition_i < dram_partitions_count);
   return *partitions[rand_partition_i];
}
// -------------------------------------------------------------------------------------
BufferFrame& BufferManager::randomBufferFrame()
{
   auto rand_buffer_i = utils::RandomGenerator::getRand<u64>(0, dram_buffer_pool_frame_num);
   return bfs[rand_buffer_i];
}
//----------------------[Added].---------------------------------------------------------------------------
// CXL Buffer Frames Management
// if cxl_tiering_enabled, always load ssd to cxl BufferFrame first, then promote to dram BufferFrame
// getCXLPartition already has dram_partitions_count offset
//---------------------------------------------------------------------------------------------------------
// [Caution]: The original logic is misleading, randomParition and randomCXLPartition can directly index.
Partition& BufferManager::randomCXLPartition()
{
   auto rand_cxl_partition_i = utils::RandomGenerator::getRand<u64>(0, cxl_partitions_count);
   u64 rand_cxl_partition_index = dram_partitions_count + rand_cxl_partition_i;
   assert(rand_cxl_partition_index < dram_partitions_count + cxl_partitions_count);
   return *partitions[rand_cxl_partition_index];
}
 
BufferFrame& BufferManager::randomCXLBufferFrame()
{
   auto rand_cxl_buffer_i = utils::RandomGenerator::getRand<u64>(0, cxl_buffer_pool_frame_num);
   return cxl_bfs[rand_cxl_buffer_i];
}

// -------------------------------------------------------------------------------------
// returns a *write locked* new buffer frame
// Create a new page, we need it immediately, so it live in DRAM.
// No conflict with cxl-tiering, no need to change this function.
BufferFrame& BufferManager::allocatePage()
{
   // Pick a pratition randomly
   Partition& partition = randomPartition();
   BufferFrame& free_bf = partition.dram_free_list.tryPop();
   PID free_pid = partition.nextPID();
   assert(free_bf.header.state == BufferFrame::STATE::FREE);
   // -------------------------------------------------------------------------------------
   // Initialize Buffer Frame
   free_bf.header.latch.assertNotExclusivelyLatched();
   free_bf.header.latch.mutex.lock();  // Exclusive lock before changing to HOT
   free_bf.header.latch->fetch_add(LATCH_EXCLUSIVE_BIT);
   free_bf.header.pid = free_pid;
   free_bf.header.state = BufferFrame::STATE::HOT;
   free_bf.header.last_written_plsn = free_bf.page.PLSN = free_bf.page.GSN = 0;
   free_bf.header.latch.assertExclusivelyLatched();
   // -------------------------------------------------------------------------------------
   COUNTERS_BLOCK() { WorkerCounters::myCounters().allocate_operations_counter++; }
   // -------------------------------------------------------------------------------------
   return free_bf;
}
// -------------------------------------------------------------------------------------
void BufferManager::evictLastPage()
{
   if (FLAGS_worker_page_eviction && last_read_bf) {
      jumpmuTry()
      {
         BMOptimisticGuard o_guard(last_read_bf->header.latch);
         const bool is_cooling_candidate = (!last_read_bf->header.keep_in_memory && !last_read_bf->header.is_being_written_back &&
                                            !(last_read_bf->header.latch.isExclusivelyLatched()) &&
                                            !last_read_bf->isDirty()
                                            // && (partition_i) >= p_begin && (partition_i) <= p_end
                                            && last_read_bf->header.state == BufferFrame::STATE::HOT);
         if (!is_cooling_candidate) {
            jumpmu::jump();
         }
         o_guard.recheck();
         // -------------------------------------------------------------------------------------
         bool picked_a_child_instead = false;
         DTID dt_id = last_read_bf->page.dt_id;
         PID last_pid = last_read_bf->header.pid;
         o_guard.recheck();
         getDTRegistry().iterateChildrenSwips(dt_id, *last_read_bf, [&](Swip<BufferFrame>&) {
            picked_a_child_instead = true;
            return false;
         });
         if (picked_a_child_instead) {
            jumpmu::jump();
         }
         // assert(!partition.io_ht.lookup(last_read_bf->header.pid));
         // assert(!partition.io_ht.lookup(pid));
         ParentSwipHandler parent_handler = getDTRegistry().findParent(dt_id, *last_read_bf);
         // -------------------------------------------------------------------------------------
         if (FLAGS_optimistic_parent_pointer) {
            if (parent_handler.is_bf_updated) {
               o_guard.guard.version += 2;
            }
         }
         // -------------------------------------------------------------------------------------
         assert(parent_handler.parent_guard.state == GUARD_STATE::OPTIMISTIC);
         o_guard.recheck();
         BMExclusiveUpgradeIfNeeded p_x_guard(parent_handler.parent_guard);
         o_guard.guard.toExclusive();
         // -------------------------------------------------------------------------------------
         assert(!last_read_bf->header.is_being_written_back);
         assert(last_read_bf->header.state != BufferFrame::STATE::FREE);
         parent_handler.swip.evict(last_pid);
         // -------------------------------------------------------------------------------------
         // Reclaim buffer frame — route to the correct tier's free list
         const bool evicted_from_cxl = FLAGS_cxl_tiering_enabled && isInCXL(last_read_bf);
         last_read_bf->reset();
         last_read_bf->header.latch->fetch_add(LATCH_EXCLUSIVE_BIT, std::memory_order_release);
         last_read_bf->header.latch.mutex.unlock();
         FreedBfsBatch freed_bfs_batch;
         freed_bfs_batch.add(*last_read_bf);
         if (evicted_from_cxl) {
            freed_bfs_batch.pushCXL(getCXLPartition(last_pid));
         } else {
            freed_bfs_batch.push(getPageID_AllocatorPartition(last_pid));
         }
         diag.evictions.fetch_add(1, std::memory_order_relaxed);
      }
      jumpmuCatch() { last_read_bf = nullptr; }
   }
}
// -------------------------------------------------------------------------------------
// Pre: bf is exclusively locked
// ATTENTION: this function unlocks it !!
// -------------------------------------------------------------------------------------
void BufferManager::reclaimPage(BufferFrame& bf)
{
   // Step 1: PID reclamation — always routes to DRAM partition (PID allocator lives there)
   const PID pid = bf.header.pid;  // save before reset() clears it
   Partition& pid_partition = getPageID_AllocatorPartition(pid);
   if (FLAGS_recycle_pages) {
      pid_partition.freePageID(pid);
   }
   // -------------------------------------------------------------------------------------
   // Step 2: BufferFrame reclamation — route to the correct tier's free list
   const bool bf_in_cxl = FLAGS_cxl_tiering_enabled && isInCXL(&bf);
   if (bf.header.is_being_written_back) {
      // DO NOTHING ! we have a garbage collector ;-)
      bf.header.latch->fetch_add(LATCH_EXCLUSIVE_BIT, std::memory_order_release);
      bf.header.latch.mutex.unlock();
   } else {
      bf.reset();
      bf.header.latch->fetch_add(LATCH_EXCLUSIVE_BIT, std::memory_order_release);
      bf.header.latch.mutex.unlock();
      if (bf_in_cxl) {
         Partition& cxl_partition = getCXLPartition(pid);
         cxl_partition.cxl_free_list.push(bf);
      } else {
         pid_partition.dram_free_list.push(bf);
      }
   }
}
// -------------------------------------------------------------------------------------
// Returns a non-latched BufferFrame, called by worker threads
BufferFrame& BufferManager::resolveSwip(Guard& swip_guard, Swip<BufferFrame>& swip_value)
{
   if (swip_value.isHOT()) {
      BufferFrame& bf = swip_value.asBufferFrame();
      swip_guard.recheck();
      return bf;
   } else if (swip_value.isCOOL()) {
      BufferFrame* bf = &swip_value.asBufferFrameMasked();
      swip_guard.recheck();
      BMOptimisticGuard bf_guard(bf->header.latch);
      BMExclusiveUpgradeIfNeeded swip_x_guard(swip_guard);  // parent
      BMExclusiveGuard bf_x_guard(bf_guard);                // child
      bf->header.state = BufferFrame::STATE::HOT;
      swip_value.warm();
      return *bf;
   }
   // -------------------------------------------------------------------------------------
   swip_guard.unlock();  // Otherwise we would get a deadlock, P->G, G->P
   const PID pid = swip_value.asPageID();
   Partition& partition = getPageID_AllocatorPartition(pid);
   JMUW<std::unique_lock<std::mutex>> g_guard(partition.ht_mutex);
   swip_guard.recheck();
   paranoid(!swip_value.isHOT());
   // ----------------------------------------------------------------------------------------------------
   auto frame_handler = partition.io_ht.lookup(pid);
   if (!frame_handler) {
      // ----------------------------------[Added].-------------------------------------------------------
      // We use the same hot-cold bit judgement, because we do not change the original
      // buffer pool eviction logic here,
      // instead, we change it in PageProviderThread
      // so the first two isHot and isCold logic stay the same. (in memory (in CXL or in DRAM))
      // [Additional].
      // if the page stay at SSD
      // we first load page from SSD to CXL, find a free CXL BufferFrame Page from cxl_free_list
      // the page's dt_id( data_instance_id) is already persisted, 
      // calling the type callback to check whether this BTreeNode is Index Pages,
      // if it is Index Page, 
      // then we free the CXL BufferFrame, allocate one free page from dram_free_list
      // copy the page to DRAM BufferFrame.
      const bool load_into_cxl = FLAGS_cxl_tiering_enabled;
      BufferFrame& bf = load_into_cxl
                           ? randomCXLPartition().cxl_free_list.tryPop()
                           : randomPartition().dram_free_list.tryPop();
      IOFrame& io_frame = partition.io_ht.insert(pid);
      bf.header.latch.assertNotExclusivelyLatched();
      // -------------------------------------------------------------------------------------
      io_frame.state = IOFrame::STATE::READING;
      io_frame.readers_counter = 1;
      // -------------------------------------------------------------------------------------
      // io_frame.mutex ownership protocol:
      //
      // jumpmu is based on setjmp/longjmp and does NOT trigger C++ stack unwinding, so
      // plain std::unique_lock RAII will NOT release the mutex on a longjmp.  JMUW wraps
      // an object and registers its destructor in jumpmu's de_stack so that jump() calls
      // it explicitly before longjmp-ing.
      //
      // Strategy:
      //   1. Lock io_frame.mutex with a plain mutex.lock() — this cannot be interrupted
      //      by jumpmu because it is not inside any jumpmuTry block at this point.
      //   2. Wrap it in a JMUW<unique_lock> with std::adopt_lock so that jumpmu's
      //      destructor stack will call unique_lock::~unique_lock() (= unlock) if any
      //      longjmp fires before we explicitly release the lock below.
      //   3. Scope the JMUW inside a block so it is destroyed (and the mutex unlocked)
      //      before we enter jumpmuTry.  This guarantees io_frame.mutex is always
      //      released regardless of whether the subsequent jumpmuTry succeeds or throws.
      //
      // Why not JMUW(mutex) directly:
      //   JMUW's constructor calls mutex.lock() and then registers the destructor.
      //   If a jumpmu fires between the lock() call and the registration, the mutex
      //   remains locked with no cleanup registered — exactly the bug we are fixing.
      io_frame.mutex.lock();
      BufferFrame* final_bf = &bf;
      {
         JMUW<std::unique_lock<std::mutex>> io_frame_lock(io_frame.mutex, std::adopt_lock);
         // -------------------------------------------------------------------------------------
         g_guard->unlock();
         // -------------------------------------------------------------------------------------
         readPageSync(pid, bf.page);
         // Count every SSD read as a miss: the page was not resident in any
         // in-memory tier (DRAM buffer pool or CXL) and had to be fetched from SSD.
         // This is the only correct place to count ssd_miss because after
         // readPageSync the frame is swizzled into the DRAM buffer pool, so
         // isInDRAM() will return true and the BTree-layer check cannot distinguish
         // a cold miss from a warm DRAM hit.
         diag.ssd_miss.fetch_add(1, std::memory_order_relaxed);
         // -------------------------------------------------------------------------------------
         paranoid(bf.header.state == BufferFrame::STATE::FREE);
         COUNTERS_BLOCK()
         {
            WorkerCounters::myCounters().dt_page_reads[bf.page.dt_id]++;
            if (FLAGS_trace_dt_id >= 0 && bf.page.dt_id == FLAGS_trace_dt_id &&
                utils::RandomGenerator::getRand<u64>(0, FLAGS_trace_trigger_probability) == 0) {
               utils::printBackTrace();
            }
         }
         paranoid(bf.page.magic_debugging_number == pid);
         // -------------------------------------------------------------------------------------
         // ATTENTION: Fill the BF header
         paranoid(!bf.header.is_being_written_back);
         bf.header.last_written_plsn = bf.page.PLSN;
         bf.header.state = BufferFrame::STATE::LOADED;
         bf.header.pid = pid;
         if (FLAGS_crc_check) {
            bf.header.crc = utils::CRC(bf.page.dt, EFFECTIVE_PAGE_SIZE);
         }
         // -------------------------------------------------------------------------------------
         // =====================================================================================
         // Index Page Logic
         // [Added].
         // Index Page should directly promote to DRAM
         // free CXL BufferFrame and reallocate a DRAM BufferFrame.
         // =====================================================================================
         if (load_into_cxl &&
            DTRegistry::global_dt_registry.shouldDirectlyPromoteToDRAM(bf.page.dt_id, bf)) {
            jumpmuTry() {
               // dram_free_list.tryPop() uses JMUW internally and can trigger a jumpmu if the
               // free list is empty. If we don't catch it, io_frame.state remains READING forever,
               // causing an infinite retry loop for any thread trying to access this page.
               BufferFrame& dram_bf = randomPartition().dram_free_list.tryPop();
               std::memcpy(&dram_bf.page, &bf.page, PAGE_SIZE);
               dram_bf.header.last_written_plsn = bf.header.last_written_plsn;
               dram_bf.header.state = BufferFrame::STATE::LOADED;
               dram_bf.header.pid = pid;
               if (FLAGS_crc_check) {
                  dram_bf.header.crc = bf.header.crc;
               }
               // Reclaim the CXL BufferFrame.
               //
               // bf is the CXL BufferFrame we loaded the page into.  While io_frame.state ==
               // READING, no other thread can reach bf via the io_ht (they block on
               // io_frame.mutex), so bf is exclusively ours — no latch is needed.
               //
               // We do NOT call bf.reset() here because reset() asserts
               // header.latch.assertExclusivelyLatched(), which would require acquiring
               // latch.mutex (a std::shared_mutex).  That acquisition can block if cxl_pp
               // holds a shared lock on the same latch while scanning CXL frames, creating
               // a deadlock: IO thread waits for latch.mutex while holding io_frame.mutex,
               // and waiting readers are blocked on io_frame.mutex.
               //
               // Instead, manually reset only the fields that reset() touches, without
               // touching the latch at all.  This is safe because bf is invisible to all
               // other threads at this point.
               bf.header.crc = 0;
               bf.header.last_writer_worker_id = std::numeric_limits<u8>::max();
               bf.header.last_written_plsn = 0;
               bf.header.state = BufferFrame::STATE::FREE;
               bf.header.is_being_written_back.store(false, std::memory_order_release);
               bf.header.pid = 9999;
               bf.header.next_free_bf = nullptr;
               bf.header.contention_tracker.reset();
               bf.header.keep_in_memory = false;
               reclaimCXLBufferFrame(bf);

               final_bf = &dram_bf;
            } jumpmuCatch() {
               // Failed to get a DRAM frame (DRAM is full). 
               // Fallback to using the CXL frame instead of aborting the IO operation.
               final_bf = &bf;
            }
         }
         // io_frame_lock goes out of scope here → unique_lock destructor unlocks
         // io_frame.mutex, JMUW destructor removes it from jumpmu's de_stack.
         // From this point on io_frame.mutex is unlocked and no longer tracked by jumpmu.
      }
      jumpmuTry()
      {
         swip_guard.recheck();
         JMUW<std::unique_lock<std::mutex>> g_guard(partition.ht_mutex);
         BMExclusiveUpgradeIfNeeded swip_x_guard(swip_guard);
         swip_value.warm(final_bf);
         final_bf->header.state = BufferFrame::STATE::HOT;  // ATTENTION: SET TO HOT AFTER
                                                             // IT IS SWIZZLED IN
         // -------------------------------------------------------------------------------------
         if (io_frame.readers_counter.fetch_add(-1) == 1) {
            partition.io_ht.remove(pid);
         }
         // -------------------------------------------------------------------------------------
         last_read_bf = final_bf;
         jumpmu_return *final_bf;
      }
      jumpmuCatch()
      {
         // io_frame.mutex is already unlocked (JMUW scope ended above).
         // Publish final_bf as READY so the next reader picks it up via STATE::READY branch.
         // Use partition.ht_mutex.lock() directly (not JMUW) to guarantee this state
         // update is never skipped by a nested jumpmu.
         partition.ht_mutex.lock();
         io_frame.bf = final_bf;
         io_frame.state = IOFrame::STATE::READY;
         partition.ht_mutex.unlock();
         // -------------------------------------------------------------------------------------
         jumpmu::jump();
      }
   }
   // -------------------------------------------------------------------------------------
   IOFrame& io_frame = frame_handler.frame();
   // -------------------------------------------------------------------------------------
   if (io_frame.state == IOFrame::STATE::READING) {
      io_frame.readers_counter++;  // incremented while holding partition lock
      g_guard->unlock();
      io_frame.mutex.lock();
      io_frame.mutex.unlock();
      if (io_frame.readers_counter.fetch_add(-1) == 1) {
         g_guard->lock();
         if (io_frame.readers_counter == 0) {
            partition.io_ht.remove(pid);
         }
         g_guard->unlock();
      }
      // -------------------------------------------------------------------------------------
      jumpmu::jump();
   }
   // -------------------------------------------------------------------------------------
   if (io_frame.state == IOFrame::STATE::READY) {
      // -------------------------------------------------------------------------------------
      BufferFrame* bf = io_frame.bf;
      {
         // We have to exclusively lock the bf because the page provider thread will
         // try to evict them when its IO is done
         bf->header.latch.assertNotExclusivelyLatched();
         paranoid(bf->header.state == BufferFrame::STATE::LOADED);
         BMOptimisticGuard bf_guard(bf->header.latch);
         BMExclusiveUpgradeIfNeeded swip_x_guard(swip_guard);
         BMExclusiveGuard bf_x_guard(bf_guard);
         // -------------------------------------------------------------------------------------
         io_frame.bf = nullptr;
         paranoid(bf->header.pid == pid);
         swip_value.warm(bf);
         paranoid(swip_value.isHOT());
         paranoid(bf->header.state == BufferFrame::STATE::LOADED);
         bf->header.state = BufferFrame::STATE::HOT;  // ATTENTION: SET TO HOT AFTER
                                                      // IT IS SWIZZLED IN
         // -------------------------------------------------------------------------------------
         if (io_frame.readers_counter.fetch_add(-1) == 1) {
            partition.io_ht.remove(pid);
         } else {
            io_frame.state = IOFrame::STATE::TO_DELETE;
         }
         g_guard->unlock();
         // -------------------------------------------------------------------------------------
         last_read_bf = bf;
         return *bf;
      }
   }
   if (io_frame.state == IOFrame::STATE::TO_DELETE) {
      if (io_frame.readers_counter == 0) {
         partition.io_ht.remove(pid);
      }
      g_guard->unlock();
      jumpmu::jump();
   }
   ensure(false);
}  // namespace storage
// -------------------------------------------------------------------------------------
// SSD management
// -------------------------------------------------------------------------------------
void BufferManager::readPageSync(u64 pid, u8* destination)
{
   paranoid(u64(destination) % 512 == 0);
   s64 bytes_left = PAGE_SIZE;
   do {
      const int bytes_read = pread(ssd_fd, destination, bytes_left, pid * PAGE_SIZE + (PAGE_SIZE - bytes_left));
      assert(bytes_read > 0);  // call was successfull?
      bytes_left -= bytes_read;
   } while (bytes_left > 0);
   // -------------------------------------------------------------------------------------
   COUNTERS_BLOCK() { WorkerCounters::myCounters().read_operations_counter++; }
}
// -------------------------------------------------------------------------------------
void BufferManager::fDataSync()
{
   fdatasync(ssd_fd);
}
// -------------------------------------------------------------------------------------
// page_id -> partition
u64 BufferManager::getPageID_AllocatorPartitionID(PID pid)
{
   return pid & dram_partitions_mask;
}
// -------------------------------------------------------------------------------------
// [Caution]:
// The originial parameter can be any page_id
// should check whether this page_id in cxl_tiering or dram_tiering
Partition& BufferManager::getPageID_AllocatorPartition(PID pid)
{
   const u64 partition_i = getPageID_AllocatorPartitionID(pid);
   assert(partition_i < dram_partitions_count);
   return *partitions[partition_i];
}
// -----------------------[Added].----------------------------------------------------------
// default partitions Array Layout:
// [0 .. 63] <- DRAM partitions (dram_partitions_count = 2^6), mask is helpful
// [64 ... 191] <- CXL partitions(cxl_partitions_count = 128), define its own rounting rule
// ------------------------------------------------------------------------------------------
u64 BufferManager::getCXLPartitionID(PID pid){
     return dram_partitions_count + (pid & cxl_partitions_mask);
}

Partition& BufferManager::getCXLPartition(PID pid){
   const u64 cxl_partition_i = getCXLPartitionID(pid);
   assert((cxl_partition_i >= dram_partitions_count)  && (cxl_partition_i < dram_partitions_count + cxl_partitions_count));
   return *partitions[cxl_partition_i];
}

// -------------------------------------------------------------------------------------
void BufferManager::stopBackgroundThreads()
{
   bg_threads_keep_running = false;
   while (bg_threads_counter) {
   }
}
// -------------------------------------------------------------------------------------
BufferManager::~BufferManager()
{
   stopBackgroundThreads();
   // -------------------------------------------------------------------------------------
   //----------------------[Added].--------------------------------------------------------
   //---------------------if cxl_tiering_enabled, munmap cxl_bfs and record_cache_ptr
   const u64 dram_buffer_pool_in_safety_bytes = sizeof(BufferFrame) * (dram_buffer_pool_frame_num + safety_pages);
   munmap(bfs, dram_buffer_pool_in_safety_bytes);

   if(FLAGS_cxl_tiering_enabled){
      if (global_record_cache != nullptr) {
         delete global_record_cache;
         global_record_cache = nullptr;
      }
      if (record_cache_allocator != nullptr) {
         delete record_cache_allocator;
         record_cache_allocator = nullptr;
      }
      if (global_admission_control != nullptr) {
         delete global_admission_control;
         global_admission_control = nullptr;
      }

      const u64 cxl_buffer_pool_in_safety_bytes = sizeof(BufferFrame) * (cxl_buffer_pool_frame_num + safety_pages);
      constexpr u64 CXL_ALIGNMENT = 2ULL * 1024 * 1024; // 2 MiB
      const u64 cxl_total_bytes = (cxl_buffer_pool_in_safety_bytes + CXL_ALIGNMENT - 1) & ~(CXL_ALIGNMENT - 1);
      munmap(cxl_bfs, cxl_total_bytes);

      // Must match the allocation size computed in the constructor:
      //   rc_budget_bytes + one extra 2 MiB slab (safety margin)
      const u64 rc_budget_bytes = static_cast<u64>(FLAGS_dram_recordcache_gib * 1024.0 * 1024.0 * 1024.0);
      const u64 rc_safety_bytes = 2ULL * 1024 * 1024;
      const u64 dram_record_cache_in_safety_bytes = rc_budget_bytes + rc_safety_bytes;
      munmap(record_cache_ptr, dram_record_cache_in_safety_bytes);
   }
}
// -------------------------------------------------------------------------------------
BufferManager* BMC::global_bf(nullptr);
}  // namespace storage
}  // namespace leanstore
// -------------------------------------------------------------------------------------
