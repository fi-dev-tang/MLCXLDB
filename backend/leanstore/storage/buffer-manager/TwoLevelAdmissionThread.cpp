#include "BufferManager.hpp"
#include "leanstore/profiling/counters/CPUCounters.hpp"
#include"leanstore/concurrency-recovery/CRMG.hpp"
#include "leanstore/profiling/counters/WorkerCounters.hpp"
#include "leanstore/utils/Misc.hpp"
#include "leanstore/utils/Parallelize.hpp"
#include "leanstore/utils/RandomGenerator.hpp"
#include<span>
#include "leanstore/storage/btree/core/BTreeNode.hpp"

namespace leanstore {
namespace storage {

void BufferManager::twoLevelAdmissionThread()
{
   pthread_setname_np(pthread_self(), "two_level_admission");

   fprintf(stdout, "[background twoLevelAdmissionThread] thread start, before register\n");
   leanstore::cr::CRManager::global->registerMeAsSpecialWorker();
   fprintf(stdout, "[background twoLevelAdmissionThread] thread registered as special worker\n");

   while (bg_threads_keep_running) {
      // 1. Call BackgroundRoutine for the global admission control
      double fill_ratio = 0.0;
      if (global_record_cache != nullptr) {
         fill_ratio = global_record_cache->GetRecordCacheFillRatio();
      }
      auto decisions = global_admission_control->GetAdmissionControl().BackgroundRoutine(fill_ratio);
      
      // 2. Process promotion decisions
      for (const auto& decision : decisions) {
         if (decision.promote_entire_page) {
            promoteFullPage(decision.page_id, decision.cxl_bf);
         } else {
            for (u16 slot_id : decision.hot_slot_ids) {
               promoteRecordCacheEntry(decision.page_id, slot_id, decision.cxl_bf);
            }
         }
      }

      // Sleep to control the background scan frequency
      usleep(1000);
   }
   // Thread is exiting, decrement counter
   bg_threads_counter.fetch_sub(1, std::memory_order_release);
}

void BufferManager::promoteFullPage(u64 page_id, BufferFrame* cxl_bf){
   jumpmuTry(){
      // Drop stale decisions instead of doing an O(N) cxl_bfs scan.
      // BackgroundRoutine reruns every ~1ms and will re-discover the page
      // with a fresh cxl_bf if it is still hot.
      if(cxl_bf == nullptr || !isInCXL(cxl_bf)){
         jumpmu_return;
      }
      if (cxl_bf->header.is_being_written_back.load(std::memory_order_acquire)) {
         jumpmu_return;
      }

      // 2. OptimisticGuard on child(cxl BufferFrame)
      BMOptimisticGuard cxl_op_guard(cxl_bf -> header.latch);

      // 3. Get dt_id and recheck
      DTID dt_id = cxl_bf -> page.dt_id;
      PID pid = cxl_bf -> header.pid;
      cxl_op_guard.recheck();

      // 4. find parent node
      ParentSwipHandler parent_handler = getDTRegistry().findParent(dt_id, *cxl_bf);

      // 5. Parent get Exclusively Locked
      BMExclusiveUpgradeIfNeeded p_x_guard(parent_handler.parent_guard);

      // 6. Child get Exclusively locked
      BMExclusiveGuard cxl_x_guard(cxl_op_guard);

      // 7. recheck child pid
      if(cxl_bf -> header.pid != page_id || cxl_bf -> header.state != BufferFrame::STATE::HOT){
         jumpmu_return;
      }
      if (cxl_bf->header.is_being_written_back.load(std::memory_order_acquire)) {
         jumpmu_return;
      }

      if(!parent_handler.swip.isHOT()){
         jumpmu_return;
      }

      if(&parent_handler.swip.asBufferFrameMasked() != cxl_bf){
         jumpmu_return;
      }

      // 8. Allocate DRAM BufferFrame
      BufferFrame& dram_bf = randomPartition().dram_free_list.tryPop();
      dram_bf.header.latch.assertNotExclusivelyLatched();
      dram_bf.header.latch.mutex.lock();
      dram_bf.header.latch -> fetch_add(LATCH_EXCLUSIVE_BIT);

      // 9. Copy context
      std::memcpy(&dram_bf.page, &cxl_bf -> page, PAGE_SIZE);
      dram_bf.header.pid = cxl_bf -> header.pid;
      dram_bf.header.state = BufferFrame::STATE::HOT;
      dram_bf.header.last_written_plsn = cxl_bf -> header.last_written_plsn;

      // 10. Update Parent Swip
      parent_handler.swip.warm(&dram_bf);

      // 11. Release DRAM frame lock
      dram_bf.header.latch -> fetch_sub(LATCH_EXCLUSIVE_BIT, std::memory_order_release);
      dram_bf.header.latch.mutex.unlock();

      // 12. Reclaim CXL BufferFrame
      cxl_bf -> reset();
      reclaimCXLBufferFrame(*cxl_bf);
      diag.cxl_to_dram_promotions.fetch_add(1, std::memory_order_relaxed);
   }jumpmuCatch(){

   }
}

void BufferManager::promoteRecordCacheEntry(u64 page_id, u16 slot_id, BufferFrame* cxl_bf){
   if(global_record_cache == nullptr) return;

   jumpmuTry(){
      // Drop stale decisions instead of doing an O(N) cxl_bfs scan.
      // BackgroundRoutine reruns every ~1ms and will re-discover the page
      // with a fresh cxl_bf if it is still hot.
      if(cxl_bf == nullptr || !isInCXL(cxl_bf) ||
         cxl_bf -> header.pid != page_id ||
         cxl_bf -> header.state != BufferFrame::STATE::HOT){
         jumpmu_return;
      }

      // 1. Optimistic Guard
      BMOptimisticGuard cxl_opt_guard(cxl_bf -> header.latch);

      // 2. Check again frame identity
      if(cxl_bf -> header.pid != page_id ||
      cxl_bf -> header.state != BufferFrame::STATE::HOT){
         jumpmu_return;
      }
      if (cxl_bf->header.is_being_written_back.load(std::memory_order_acquire)) {
         jumpmu_return;
      }

      auto *node = reinterpret_cast<btree::BTreeNode*>(cxl_bf -> page.dt);
      if(node == nullptr){
         jumpmu_return;
      }

      // boundary check
      const u16 count = node -> count;
      if(slot_id >= count){
         jumpmu_return;
      }

      // Key_len must fit boundary
      u16 full_key_len = node -> getFullKeyLen(slot_id);
      if(full_key_len == 0 || full_key_len > PAGE_SIZE){
         jumpmu_return;
      }

      std::vector<u8> full_key(full_key_len);

      // Copy key / payload
      node -> copyFullKey(slot_id, full_key.data());
      u16 value_len = node -> getPayloadLength(slot_id);

      // final check
      cxl_opt_guard.recheck();

      // send it to promote thread
      std::span<const u8> key_span(full_key.data(), full_key.size());
      global_record_cache -> signalPromoteThread(key_span, cxl_bf, page_id, slot_id, value_len);
   }jumpmuCatch(){

   }
}

}  // namespace storage
}  // namespace leanstore