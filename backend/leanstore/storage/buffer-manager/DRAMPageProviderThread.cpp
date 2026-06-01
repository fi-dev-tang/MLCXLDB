#include "AsyncWriteBuffer.hpp"
#include "BufferFrame.hpp"
#include "BufferManager.hpp"
#include "Exceptions.hpp"
#include "Tracing.hpp"
#include "leanstore/Config.hpp"
#include "leanstore/concurrency-recovery/CRMG.hpp"
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
// -------------------------------------------------------------------------------------
namespace leanstore
{
namespace storage
{
// =====================================================================================
// dramPageProviderThread  (CXL-tiering enabled path)
//
// Responsibility: reclaim DRAM BufferFrames by demoting pages to CXL.
//
// Phase 1 — DRAM Cooling: randomly sample DRAM BFs, transition HOT → COOL.
//           (logic identical to the original pageProviderThread Phase 1)
//
// Phase 2 — DRAM Demotion: for every COOL DRAM page (dirty or clean, leaf or index),
//           demote it to a free CXL BufferFrame.
//           No SSD I/O is performed here — dirty state travels with the page data
//           (page.PLSN / header.last_written_plsn).
//
// Concurrency during demotion (three locks):
//   1. Parent node       — exclusive lock (BMExclusiveUpgradeIfNeeded)
//   2. Current DRAM BF   — exclusive lock (guard.toExclusive)
//   3. CXL BF            — freshly popped from cxl_free_list, invisible to others
//
// After demotion the CXL BF is set to HOT and the parent Swip is warm()'d to
// point at the CXL BF.  The DRAM BF is reset and returned to dram_free_list.
// =====================================================================================
void BufferManager::dramPageProviderThread(u64 p_begin, u64 p_end)  // [p_begin, p_end)
{
   std::string thread_name("dram_pp_" + std::to_string(p_begin) + "_" + std::to_string(p_end));
   pthread_setname_np(pthread_self(), thread_name.c_str());
   using Time = decltype(std::chrono::high_resolution_clock::now());
   // -------------------------------------------------------------------------------------
   leanstore::cr::CRManager::global->registerMeAsSpecialWorker();
   // -------------------------------------------------------------------------------------
   // No AsyncWriteBuffer needed — DRAM demotion never writes to SSD.
   std::vector<BufferFrame*> dram_cool_candidate_bfs, demotion_to_cxl_candidate_bfs;
   // -------------------------------------------------------------------------------------
   auto next_bf_range = [&]() {
      const u64 BATCH_SIZE = FLAGS_replacement_chunk_size;
      dram_cool_candidate_bfs.clear();
      for (u64 i = 0; i < BATCH_SIZE; i++) {
         BufferFrame* r_bf = &randomBufferFrame();
         DO_NOT_OPTIMIZE(r_bf->header.state);
         dram_cool_candidate_bfs.push_back(r_bf);
      }
   };
   // -------------------------------------------------------------------------------------
   while (bg_threads_keep_running) {
      // =================================================================================
      // Phase 1: DRAM Cooling — HOT → COOL
      // (identical to the original pageProviderThread Phase 1)
      // =================================================================================
      [[maybe_unused]] Time phase_1_begin, phase_1_end;
      COUNTERS_BLOCK() { phase_1_begin = std::chrono::high_resolution_clock::now(); }
      volatile u64 failed_attempts = 0;
#define repickIf(cond)                       \
   if (cond) {                               \
      failed_attempts = failed_attempts + 1; \
      jumpmu_continue;                       \
   }
      auto& current_partition = *partitions[p_begin + utils::RandomGenerator::getRand<u64>(0, p_end - p_begin)];
      // Backoff: no memory pressure and no pending demotion work → sleep to avoid busy-polling
      if (current_partition.dram_free_list.counter >= current_partition.free_bfs_limit &&
          demotion_to_cxl_candidate_bfs.empty()) {
         usleep(100);
         continue;
      }
      if ((current_partition.dram_free_list.counter < current_partition.free_bfs_limit) && failed_attempts < 10) {
         next_bf_range();
         while (dram_cool_candidate_bfs.size()) {
            jumpmuTry()
            {
               BufferFrame* r_buffer = dram_cool_candidate_bfs.back();
               dram_cool_candidate_bfs.pop_back();
               COUNTERS_BLOCK() { PPCounters::myCounters().phase_1_counter++; }
               // -------------------------------------------------------------------------------------
               BMOptimisticGuard r_guard(r_buffer->header.latch);
               repickIf(r_buffer->header.keep_in_memory || r_buffer->header.is_being_written_back || r_buffer->header.latch.isExclusivelyLatched());
               r_guard.recheck();
               // -------------------------------------------------------------------------------------
               if (r_buffer->header.state == BufferFrame::STATE::COOL) {
                  demotion_to_cxl_candidate_bfs.push_back(reinterpret_cast<BufferFrame*>(r_buffer));
                  repickIf(true);
               }
               repickIf(r_buffer->header.state != BufferFrame::STATE::HOT);
               r_guard.recheck();
               // -------------------------------------------------------------------------------------
               COUNTERS_BLOCK() { PPCounters::myCounters().touched_bfs_counter++; }
               // -------------------------------------------------------------------------------------
               bool all_children_evicted = true;
               bool picked_a_child_instead = false;
               [[maybe_unused]] Time iterate_children_begin, iterate_children_end;
               COUNTERS_BLOCK() { iterate_children_begin = std::chrono::high_resolution_clock::now(); }
               getDTRegistry().iterateChildrenSwips(r_buffer->page.dt_id, *r_buffer, [&](Swip<BufferFrame>& swip) {
                  all_children_evicted &= swip.isEVICTED();
                  // [Added].
                  // Origin: If the chosen cooling node has a hot child node, then ignore it and push its hot child into cooling bfs
                  // Current: If the chosen cooling node has a hot child node(and the child node lie in DRAM), then ignore it and push its hot child into cooling bfs.
                  if (swip.isHOT()) {
                     BufferFrame* picked_child_bf = &swip.asBufferFrame();
                     r_guard.recheck();
                     if(isInDRAM(picked_child_bf)){  // Must stay in DRAM Layer.
                        picked_a_child_instead = true;
                        dram_cool_candidate_bfs.push_back(picked_child_bf);
                        return false;
                     }
                  }
                  r_guard.recheck();
                  return true;
               });
               COUNTERS_BLOCK()
               {
                  iterate_children_begin = std::chrono::high_resolution_clock::now();
                  PPCounters::myCounters().iterate_children_ms +=
                      (std::chrono::duration_cast<std::chrono::microseconds>(iterate_children_end - iterate_children_begin).count());
               }
               repickIf(!all_children_evicted || picked_a_child_instead);
               // -------------------------------------------------------------------------------------
               [[maybe_unused]] Time find_parent_begin, find_parent_end;
               COUNTERS_BLOCK() { find_parent_begin = std::chrono::high_resolution_clock::now(); }
               DTID dt_id = r_buffer->page.dt_id;
               r_guard.recheck();
               ParentSwipHandler parent_handler = getDTRegistry().findParent(dt_id, *r_buffer);
               // -------------------------------------------------------------------------------------
               if (FLAGS_optimistic_parent_pointer) {
                  if (parent_handler.is_bf_updated) {
                     r_guard.guard.version += 2;
                  }
               }
               // -------------------------------------------------------------------------------------
               paranoid(parent_handler.parent_guard.state == GUARD_STATE::OPTIMISTIC);
               paranoid(parent_handler.parent_guard.latch != reinterpret_cast<HybridLatch*>(0x99));
               COUNTERS_BLOCK()
               {
                  find_parent_end = std::chrono::high_resolution_clock::now();
                  PPCounters::myCounters().find_parent_ms +=
                      (std::chrono::duration_cast<std::chrono::microseconds>(find_parent_end - find_parent_begin).count());
               }
               // -------------------------------------------------------------------------------------
               r_guard.recheck();
               const SpaceCheckResult space_check_res = getDTRegistry().checkSpaceUtilization(r_buffer->page.dt_id, *r_buffer);
               if (space_check_res == SpaceCheckResult::RESTART_SAME_BF || space_check_res == SpaceCheckResult::PICK_ANOTHER_BF) {
                  jumpmu_continue;
               }
               r_guard.recheck();
               // -------------------------------------------------------------------------------------
               // Suitable page found, cool it: HOT → COOL
               {
                  const PID pid = r_buffer->header.pid;
                  {
                     BMExclusiveUpgradeIfNeeded p_x_guard(parent_handler.parent_guard);
                     BMExclusiveGuard r_x_guard(r_guard);
                     // -------------------------------------------------------------------------------------
                     paranoid(r_buffer->header.pid == pid);
                     paranoid(r_buffer->header.state == BufferFrame::STATE::HOT);
                     paranoid(r_buffer->header.is_being_written_back == false);
                     paranoid(parent_handler.parent_guard.version == parent_handler.parent_guard.latch->ref().load());
                     paranoid(parent_handler.swip.bf == r_buffer);
                     // -------------------------------------------------------------------------------------
                     r_buffer->header.state = BufferFrame::STATE::COOL;
                     parent_handler.swip.cool();
                  }
                  COUNTERS_BLOCK() { PPCounters::myCounters().unswizzled_pages_counter++; }
               }
               failed_attempts = 0;
            }
            jumpmuCatch() {}
         }
      }
      COUNTERS_BLOCK()
      {
         phase_1_end = std::chrono::high_resolution_clock::now();
         PPCounters::myCounters().phase_1_ms += (std::chrono::duration_cast<std::chrono::microseconds>(phase_1_end - phase_1_begin).count());
      }
      // =================================================================================
      // Phase 2: DRAM Demotion — demote every COOL DRAM page to CXL
      //
      // All COOL pages are demoted regardless of dirty/clean or leaf/index.
      // Dirty state (page.PLSN vs header.last_written_plsn) travels with the data;
      // the CXL eviction path (cxlPageProviderThread) will handle flushing to SSD.
      // =================================================================================
      FreedBfsBatch freed_bfs_batch;

      auto demote_bf_to_cxl = [&](BufferFrame& dram_bf, BMOptimisticGuard& c_guard) {
         DTID dt_id = dram_bf.page.dt_id;
         c_guard.recheck();
         ParentSwipHandler parent_handler = getDTRegistry().findParent(dt_id, dram_bf);
         // -------------------------------------------------------------------------------------
         if (FLAGS_optimistic_parent_pointer) {
            if (parent_handler.is_bf_updated) {
               c_guard.guard.version += 2;
            }
         }
         // -------------------------------------------------------------------------------------
         paranoid(parent_handler.parent_guard.state == GUARD_STATE::OPTIMISTIC);

         // Obtain a free CXL BufferFrame; tryPop() throws jumpmu if CXL pool is empty.
         BufferFrame& cxl_bf = randomCXLPartition().cxl_free_list.tryPop();

         // === Three locks ===
         // Lock 1: Parent — exclusive
         BMExclusiveUpgradeIfNeeded p_x_guard(parent_handler.parent_guard);
         // Lock 2: Current DRAM BF — exclusive
         // Must use tryToExclusive() (non-blocking) instead of toExclusive() (blocking).
         // toExclusive() calls mutex.lock() which can block while already holding the
         // parent lock (p_x_guard), creating a lock-order deadlock with worker threads
         // that acquire child-then-parent in resolveSwip's isCOOL() path.
         c_guard.guard.tryToExclusive();
         // Lock 3: CXL BF — just popped from free list, no contention
         // -------------------------------------------------------------------------------------
         if (FLAGS_crc_check && dram_bf.header.crc) {
            ensure(utils::CRC(dram_bf.page.dt, EFFECTIVE_PAGE_SIZE) == dram_bf.header.crc);
         }
         // -------------------------------------------------------------------------------------
         paranoid(!dram_bf.header.is_being_written_back);
         paranoid(dram_bf.header.state == BufferFrame::STATE::COOL);
         paranoid(parent_handler.swip.isCOOL());

         const PID demoted_pid = dram_bf.header.pid;

         // --- Copy page data + header metadata to CXL BF ---
         std::memcpy(&cxl_bf.page, &dram_bf.page, PAGE_SIZE);
         cxl_bf.header.pid = demoted_pid;
         cxl_bf.header.last_written_plsn = dram_bf.header.last_written_plsn;
         cxl_bf.header.last_writer_worker_id = dram_bf.header.last_writer_worker_id;
         cxl_bf.header.keep_in_memory = dram_bf.header.keep_in_memory;
         cxl_bf.header.contention_tracker = dram_bf.header.contention_tracker;
         if (FLAGS_crc_check) {
            cxl_bf.header.crc = dram_bf.header.crc;
         }
         // CXL BF becomes HOT — Swip semantics are identical
         cxl_bf.header.state = BufferFrame::STATE::HOT;

         // --- Atomic Swip switch: parent now points to CXL BF (as HOT) ---
         parent_handler.swip.warm(&cxl_bf);

         // --- Reclaim DRAM BufferFrame ---
         dram_bf.reset();
         // Release the exclusive latch via the guard's unlock() so that version
         // and mutex are updated atomically and consistently.  Do NOT call
         // fetch_add(LATCH_EXCLUSIVE_BIT) + mutex.unlock() here: toExclusive()
         // already incremented the version and locked the mutex; a second manual
         // fetch_add would flip the exclusive bit back to 0 prematurely, and the
         // subsequent guard destructor would then re-set the bit and double-unlock
         // the mutex (UB), leaving the latch permanently in the exclusive state.
         c_guard.guard.unlock();

         freed_bfs_batch.add(dram_bf);
         // [Added]. [PullRequest]: The original logic is wrong, should accumulate util reach 128
         // That is why batch pushing works!
         if (freed_bfs_batch.size() >= std::min<u64>(FLAGS_worker_threads, 128)) {
            freed_bfs_batch.push(current_partition);
         }
         // -------------------------------------------------------------------------------------
         if (FLAGS_pid_tracing) {
            Tracing::mutex.lock();
            if (Tracing::ht.contains(demoted_pid)) {
               std::get<1>(Tracing::ht[demoted_pid])++;
            } else {
               Tracing::ht[demoted_pid] = {dt_id, 1};
            }
            Tracing::mutex.unlock();
         }
         // -------------------------------------------------------------------------------------
         COUNTERS_BLOCK() { PPCounters::myCounters().evicted_pages++; }
      };
      // -------------------------------------------------------------------------------------
      // Iterate COOL candidates and demote each to CXL
      // -------------------------------------------------------------------------------------
      for (volatile const auto& cooled_bf : demotion_to_cxl_candidate_bfs) {
         jumpmuTry()
         {
            BMOptimisticGuard o_guard(cooled_bf->header.latch);
            if (cooled_bf->header.state != BufferFrame::STATE::COOL ||
                cooled_bf->header.is_being_written_back) {
               jumpmu_continue;
            }
            const PID cooled_bf_pid = cooled_bf->header.pid;
            // io_ht lives in DRAM partitions — routing logic unchanged
            {
               Partition& partition = getPageID_AllocatorPartition(cooled_bf_pid);
               JMUW<std::unique_lock<std::mutex>> io_guard(partition.ht_mutex);
               if (partition.io_ht.lookup(cooled_bf_pid)) {
                  jumpmu_continue;
               }
            }
            // Demote to CXL (whether dirty or clean, leaf or index)
            demote_bf_to_cxl(*cooled_bf, o_guard);
         }
         jumpmuCatch() {}
      }
      demotion_to_cxl_candidate_bfs.clear();

      // No Phase 3 — DRAM demotion never writes to SSD.
      if (freed_bfs_batch.size()) {
         freed_bfs_batch.push(current_partition);
      }
      COUNTERS_BLOCK() { PPCounters::myCounters().pp_thread_rounds++; }
   }
   bg_threads_counter--;
}
// -------------------------------------------------------------------------------------
}  // namespace storage
}  // namespace leanstore
