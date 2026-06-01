#include "BufferManager.hpp"
#include "leanstore/profiling/counters/CPUCounters.hpp"
#include"leanstore/concurrency-recovery/CRMG.hpp"
#include "leanstore/profiling/counters/WorkerCounters.hpp"
#include "leanstore/utils/Misc.hpp"
#include "leanstore/utils/Parallelize.hpp"
#include "leanstore/utils/RandomGenerator.hpp"
#include<span>
#include "leanstore/storage/btree/core/BTreeNode.hpp"
#include "leanstore/storage/btree/BTreeVI.hpp"

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
      
      // 2. Process promotion/demotion decisions
      using Action = two_level_admission_control::DramHotPageCandidates::PromotionDecision::Action;
      for (const auto& decision : decisions) {
         switch (decision.action) {
            case Action::PROMOTE_FULL_PAGE:
               promoteFullPage(decision.page_id, decision.cxl_bf);
               break;
            case Action::PROMOTE_RECORDS:
               for (u16 slot_id : decision.hot_slot_ids) {
                  promoteRecordCacheEntry(decision.page_id, slot_id, decision.cxl_bf);
               }
               break;
            case Action::DEMOTE_FULL_PAGE:
               demoteFullPage(decision.cxl_bf);
               break;
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

void BufferManager::demoteFullPage(BufferFrame* dram_bf){
   jumpmuTry(){
      if(dram_bf == nullptr || isInCXL(dram_bf)){
         jumpmu_return;
      }
      if (dram_bf->header.is_being_written_back.load(std::memory_order_acquire)) {
         jumpmu_return;
      }

      BMOptimisticGuard dram_op_guard(dram_bf->header.latch);

      DTID dt_id = dram_bf->page.dt_id;
      PID pid = dram_bf->header.pid;
      dram_op_guard.recheck();

      if (dram_bf->header.state != BufferFrame::STATE::HOT) {
         jumpmu_return;
      }

      ParentSwipHandler parent_handler = getDTRegistry().findParent(dt_id, *dram_bf);

      BMExclusiveUpgradeIfNeeded p_x_guard(parent_handler.parent_guard);

      dram_op_guard.guard.tryToExclusive();

      if (dram_bf->header.pid != pid || dram_bf->header.state != BufferFrame::STATE::HOT) {
         jumpmu_return;
      }
      if (dram_bf->header.is_being_written_back.load(std::memory_order_acquire)) {
         jumpmu_return;
      }
      if (&parent_handler.swip.asBufferFrameMasked() != dram_bf) {
         jumpmu_return;
      }

      BufferFrame& cxl_bf = randomCXLPartition().cxl_free_list.tryPop();

      std::memcpy(&cxl_bf.page, &dram_bf->page, PAGE_SIZE);
      cxl_bf.header.pid = dram_bf->header.pid;
      cxl_bf.header.last_written_plsn = dram_bf->header.last_written_plsn;
      cxl_bf.header.last_writer_worker_id = dram_bf->header.last_writer_worker_id;
      cxl_bf.header.state = BufferFrame::STATE::HOT;

      parent_handler.swip.warm(&cxl_bf);

      dram_bf->reset();
      dram_op_guard.guard.unlock();
      randomPartition().dram_free_list.push(*dram_bf);

      diag.dram_to_cxl_demotions.fetch_add(1, std::memory_order_relaxed);
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
      u16 full_payload_len = node -> getPayloadLength(slot_id);
      if (full_payload_len < sizeof(btree::BTreeVI::ChainedTuple)) {
         jumpmu_return;
      }
      u16 value_len = full_payload_len - sizeof(btree::BTreeVI::ChainedTuple);

      // [N4-DEFENSE] Large-Record guard: when a page holds only 2–4 records
      // (e.g. 16 KB page at 50% fill → record size ≥ 2 KB), the per-record-cache
      // footprint approaches per-page footprint. Slab-classing such records also
      // wastes a 2 MB slab on a handful of blocks (the 4 KB / 8 KB / 16 KB size
      // classes have only 512 / 256 / 128 blocks per slab, so a few admissions
      // can consume an entire slab before SIEVE has any chance to recycle).
      // Route directly to promoteFullPage instead — the whole page becomes hot
      // in DRAM and we skip the slab allocator entirely.
      //
      // Threshold = PAGE_SIZE / 8 = 2 KB. This is the boundary the user defined:
      // "a page that actually only has 2–4 records, including 16 KB at 50% fill".
      constexpr u16 kLargeRecordPromoteFullThreshold = PAGE_SIZE / 8;
      if(value_len >= kLargeRecordPromoteFullThreshold){
         cxl_opt_guard.recheck();
         // Hand off to full-page promotion. promoteFullPage runs its own
         // OptimisticGuard → ExclusiveGuard sequence on the same cxl_bf and
         // is safe to call from here (jumpmu nesting is supported up to
         // JUMPMU_STACK_SIZE=100; we are at depth 2).
         promoteFullPage(page_id, cxl_bf);
         jumpmu_return;
      }

      // Read dt_id from the page header inside the optimistic guard so the
      // RecordCache key gets namespaced by table identity (see signalPromoteThread).
      const u16 page_dt_id = static_cast<u16>(cxl_bf->page.dt_id);

      // final check
      cxl_opt_guard.recheck();

      // send it to promote thread
      std::span<const u8> key_span(full_key.data(), full_key.size());
      global_record_cache -> signalPromoteThread(page_dt_id, key_span, cxl_bf, page_id, slot_id, value_len);
   }jumpmuCatch(){

   }
}

}  // namespace storage
}  // namespace leanstore