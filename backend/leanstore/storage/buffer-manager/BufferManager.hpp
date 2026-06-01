#pragma once
#include "BMPlainGuard.hpp"
#include "BufferFrame.hpp"
#include "DTRegistry.hpp"
#include "FreeList.hpp"
#include "Partition.hpp"
#include "Swip.hpp"
#include "Units.hpp"
#include "leanstore/storage/two-level-admission-control/TwoLevelAdmissionControl.hpp"
#include "leanstore/storage/record-cache/RecordCache.hpp"
// -------------------------------------------------------------------------------------
#include "PerfEvent.hpp"
// -------------------------------------------------------------------------------------
#include <libaio.h>
#include <sys/mman.h>

#include <cstring>
#include <list>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
// -------------------------------------------------------------------------------------
namespace leanstore
{
class LeanStore;  // Forward declaration
namespace profiling
{
class BMTable;  // Forward declaration
}
namespace storage
{
// -------------------------------------------------------------------------------------
struct FreedBfsBatch {
   BufferFrame *freed_bfs_batch_head = nullptr, *freed_bfs_batch_tail = nullptr;
   u64 freed_bfs_counter = 0;
   // -------------------------------------------------------------------------------------
   void reset()
   {
      freed_bfs_batch_head = nullptr;
      freed_bfs_batch_tail = nullptr;
      freed_bfs_counter = 0;
   }
   // -------------------------------------------------------------------------------------
   void push(Partition& partition)
   {
      partition.dram_free_list.batchPush(freed_bfs_batch_head, freed_bfs_batch_tail, freed_bfs_counter);
      reset();
   }
   // [Added]. Push freed CXL buffer frames back to the CXL free list
   void pushCXL(Partition& partition)
   {
      partition.cxl_free_list.batchPush(freed_bfs_batch_head, freed_bfs_batch_tail, freed_bfs_counter);
      reset();
   }
   // -------------------------------------------------------------------------------------
   u64 size() { return freed_bfs_counter; }
   // -------------------------------------------------------------------------------------
   void add(BufferFrame& bf)
   {
      bf.header.next_free_bf = freed_bfs_batch_head;
      if (freed_bfs_batch_head == nullptr) {
         freed_bfs_batch_tail = &bf;
      }
      freed_bfs_batch_head = &bf;
      freed_bfs_counter++;
      // -------------------------------------------------------------------------------------
   }
};
// -------------------------------------------------------------------------------------
// TODO: revisit the comments after switching to clock replacement strategy
// Notes on Synchronization in Buffer Manager
// Terminology: PPT: Page Provider Thread, WT: Worker Thread. P: Parent, C: Child, M: Cooling stage mutex
// Latching order for all PPT operations (unswizzle, evict): M -> P -> C
// Latching order for all WT operations: swizzle: [unlock P ->] M -> P ->C, coolPage: P -> C -> M
// coolPage conflict with this order which could lead to a deadlock which we can mitigate by jumping instead of blocking in BMPlainGuard [WIP]
// -------------------------------------------------------------------------------------
struct RecordCacheEntry{
   u8 temporary_placeholder;    // [TODO]: Only declared here to pass compile of BufferManager()'s sizeof(RecordCacheEntry).
};

class BufferManager
{
  private:
   friend class leanstore::LeanStore;
   friend class leanstore::profiling::BMTable;
   // -------------------------------------------------------------------------------------
   BufferFrame* bfs;                                     // Reserved: memory for dram buffer pool and dram recordcache
   RecordCacheEntry *record_cache_ptr = nullptr;         // Newly Added! RecordCacheEntry and BufferFrame has different size.
   BufferFrame* cxl_bfs = nullptr;                       // Newly Added! if cxl_tiering_enabled, start from cxl_bfs
   // -------------------------------------------------------------------------------------
   const int ssd_fd;
   // -------------------------------------------------------------------------------------
   // Free  Pages
   const u8 safety_pages = 10;                           // we reserve these extra pages to avoid segment faults
   const u8 safety_record_cache_entries = 10;            // [Added]. reserve these extra record_cache_entries to avoid segment faults
   //-------------[Added]. dram_buffer_pool_size, dram_recordcache, cxl_buffer_pool_size --------------
   u64 dram_buffer_pool_frame_num;                       // Exclude safety pages
   u64 dram_record_cache_entries_num = 0;                // Exclude safety record cache entries
   u64 cxl_buffer_pool_frame_num = 0;                    // Exclude safety pages

   atomic<u64> ssd_freed_pages_counter = 0;  // used to track how many pages did we really allocate
   // -------------------------------------------------------------------------------------
   // For cooling and inflight io
   u64 dram_partitions_count;                  // [Added]. change from partitions_count to two variables
   u64 cxl_partitions_count;

   // Separation of dram partitions routing and cxl partitions routing
   u64 dram_partitions_mask;                  // [Added]. Used for getPageID_AllocatorPartition
   u64 cxl_partitions_mask;                   // [Added]. Used for getCXLPartition

   u64 partitions_count;                     

   std::vector<std::unique_ptr<Partition>> partitions;     // Using one partitions to shard dram_buffer_pool and cxl_buffer_pool
   
   std::atomic<u64> clock_cursor = 0;

   // -----------------------------------------------------------------------------------------------
   // Threads managements
   // Three page-provider variants; startBackgroundThreads() selects based on FLAGS_cxl_tiering_enabled
   void pageProviderThread(u64 p_begin, u64 p_end);       // [p_begin, p_end)  Original: DRAM → SSD  (CXL disabled path, untouched)
   void dramPageProviderThread(u64 p_begin, u64 p_end);   // [p_begin, p_end)  DRAM cooling + demotion to CXL
   void cxlPageProviderThread(u64 p_begin, u64 p_end);    // [p_begin, p_end)  CXL cooling + eviction to SSD
   void twoLevelAdmissionThread();                        // Global Two-level admission control thread
   void startAdmissionAndRecordCacheThreads();            // Start two-level admission + record cache threads once
   //----------------------------------------[Added].-------------------------------------------------
   // PromoteFullPage or PromoteRecordCacheEntry
   void promoteFullPage(u64 page_id, BufferFrame* cxl_bf = nullptr);
   void promoteRecordCacheEntry(u64 page_id, u16 slot_id, BufferFrame* cxl_bf = nullptr);

   atomic<u64> bg_threads_counter = 0;
   atomic<bool> bg_threads_keep_running = true;
   std::atomic<bool> admission_recordcache_threads_started = false;
   // -------------------------------------------------------------------------------------
   // Misc
   Partition& randomPartition();
   BufferFrame& randomBufferFrame();
   // [Added]. ,if cxl_tiering_enabled, first allocate from cxl
   Partition& randomCXLPartition();
   BufferFrame& randomCXLBufferFrame();
   
   //====================================[Added].=====================================================
   // Refactor the routing logic:
   // getPageID_AllocatorPartition handles mapping dram_memory_address to the first DRAM portion of the partitions
   // getCXLPartition handles mapping cxl_memory_address to the CXL portion of the partitions
   //==================================================================================================
   Partition& getPageID_AllocatorPartition(PID);
   u64 getPageID_AllocatorPartitionID(PID);
   //====================[Added].==========================================================
   // [TODO]: cxl partition logic
   Partition& getCXLPartition(PID);
   u64 getCXLPartitionID(PID);

   // -------------------------------------------------------------------------------------
   // Temporary hack: let workers evict the last page they used
   static thread_local BufferFrame* last_read_bf;

//=============================[Added].=================================================================
// Global RecordCache + Admission Control (public for BTree access)
//======================================================================================================
public:
   recordcache::RecordCache *global_record_cache = nullptr;
   recordcache::RecordCacheSlabAllocator* record_cache_allocator = nullptr;

   two_level_admission_control::TwoLevelAdmissionControlWrapper* global_admission_control = nullptr;

   two_level_admission_control::TwoLevelAdmissionControl& getGlobalAdmissionControl() {
      return global_admission_control->GetAdmissionControl();
   }

  public:
   // -------------------------------------------------------------------------------------
   // [Added]. CXL Tiering: O(1) tier membership check via pointer range comparison.
   // bfs and cxl_bfs are from separate mmap() calls so their ranges never overlap.
   // -------------------------------------------------------------------------------------
   inline bool isInCXL(const BufferFrame* bf) const
   {
      if(bf == nullptr || !FLAGS_cxl_tiering_enabled || cxl_bfs == nullptr) return false;
      return bf >= cxl_bfs && bf < cxl_bfs + cxl_buffer_pool_frame_num;
   }
   inline bool isInDRAM(const BufferFrame* bf) const
   {
      if(bf == nullptr || bfs == nullptr) return false;
      return bf >= bfs && bf < bfs + dram_buffer_pool_frame_num;
   }

   // Return BufferFrame to its corresponding Partition
   void reclaimCXLBufferFrame(BufferFrame& bf){
      u64 frame_index = &bf - cxl_bfs;
      u64 cxl_partition_offset = frame_index % cxl_partitions_count;
      partitions[dram_partitions_count + cxl_partition_offset] -> cxl_free_list.push(bf);
   }
   // -------------------------------------------------------------------------------------
   // -------------------------------------------------------------------------------------
   BufferManager(s32 ssd_fd);
   ~BufferManager();
   // -------------------------------------------------------------------------------------
   BufferFrame& allocatePage();
   inline BufferFrame& tryFastResolveSwip(Guard& swip_guard, Swip<BufferFrame>& swip_value)
   {
      if (swip_value.isHOT()) {
         BufferFrame& bf = swip_value.asBufferFrame();
         swip_guard.recheck();
         return bf;
      } else {
         return resolveSwip(swip_guard, swip_value);
      }
   }
   BufferFrame& resolveSwip(Guard& swip_guard, Swip<BufferFrame>& swip_value);
   void evictLastPage();
   void reclaimPage(BufferFrame& bf);
   // -------------------------------------------------------------------------------------
   /*
    * Life cycle of a fix:
    * 1- Check if the pid is swizzled, if yes then store the BufferFrame address
    * temporarily 2- if not, then posix_check if it exists in cooling stage
    * queue, yes? remove it from the queue and return the buffer frame 3- in
    * anycase, posix_check if the threshold is exceeded, yes ? unswizzle a random
    * BufferFrame (or its children if needed) then add it to the cooling stage.
    */
   // -------------------------------------------------------------------------------------
   void readPageSync(PID pid, u8* destination);
   void readPageAsync(PID pid, u8* destination, std::function<void()> callback);
   void fDataSync();
   // -------------------------------------------------------------------------------------
   void startBackgroundThreads();
   void enableAdmissionAndRecordCacheThreads();
   void stopBackgroundThreads();
   void writeAllBufferFrames();
   std::unordered_map<std::string, std::string> serialize();
   void deserialize(std::unordered_map<std::string, std::string> map);
   // -------------------------------------------------------------------------------------
   u64 getPoolSize() { return dram_buffer_pool_frame_num;}
   u64 getCXLPoolSize() {return cxl_buffer_pool_frame_num;}
   DTRegistry& getDTRegistry() { return DTRegistry::global_dt_registry; }
   u64 inUsedPageIDCount();
   // -------------------------------------------------------------------------------------
   // Diagnostic accessors for integration testing
   // -------------------------------------------------------------------------------------
   BufferFrame* getDRAMBFs() { return bfs; }
   BufferFrame* getCXLBFs() { return cxl_bfs; }
   u64 getDRAMPartitionsCount() const { return dram_partitions_count; }
   u64 getCXLPartitionsCount() const { return cxl_partitions_count; }
   u64 getTotalPartitionsCount() const { return partitions_count; }
   const std::vector<std::unique_ptr<Partition>>& getPartitions() const { return partitions; }
   two_level_admission_control::TwoLevelAdmissionControlWrapper* getAdmissionControlWrapper() { return global_admission_control; }

   struct DiagCounters {
      std::atomic<u64> record_cache_hit{0};
      std::atomic<u64> record_cache_miss{0};
      std::atomic<u64> dram_buffer_pool_hit{0};
      std::atomic<u64> cxl_buffer_pool_hit{0};
      // ssd_miss: page was not in any in-memory tier (DRAM or CXL) and had
      // to be fetched from SSD.  Together with dram_buffer_pool_hit and
      // cxl_buffer_pool_hit this gives a complete picture of where each
      // lookup was served from:
      //   total_requests = dram_hit + cxl_hit + ssd_miss
      //   DRAM HR = dram_hit / total_requests
      //   CXL  HR = cxl_hit  / total_requests
      //   SSD miss rate = ssd_miss / total_requests
      std::atomic<u64> ssd_miss{0};
      std::atomic<u64> cxl_to_dram_promotions{0};
      std::atomic<u64> evictions{0};
   } diag;
};                                                    // namespace storage
// -------------------------------------------------------------------------------------
class BMC
{
  public:
   static BufferManager* global_bf;
};
}  // namespace storage
}  // namespace leanstore
// -------------------------------------------------------------------------------------
