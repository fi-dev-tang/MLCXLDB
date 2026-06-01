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
// cxlPageProviderThread  (CXL-tiering enabled path)
//
// Responsibility: reclaim CXL BufferFrames by evicting pages to SSD.
//
// Phase 1 — CXL Cooling: randomly sample CXL BFs, transition HOT_CXL → COOL_CXL.
// Phase 2 — CXL Eviction:
//              COOL_CXL + clean  → evict directly (Swip → evicted PID)
//              COOL_CXL + dirty  → async write to SSD
// Phase 3 — Handle async write completions;
//              after flush, if still COOL and now clean → evict and reclaim CXL BF.
//
// io_ht lookups still route to DRAM partitions (getPageID_AllocatorPartition).
// Freed CXL BFs go back to cxl_free_list via FreedBfsBatch::pushCXL().
// =====================================================================================
void BufferManager::cxlPageProviderThread(u64 p_begin, u64 p_end)  // [p_begin, p_end)
{
   std::string thread_name("cxl_pp_" + std::to_string(p_begin) + "_" + std::to_string(p_end));
   pthread_setname_np(pthread_self(), thread_name.c_str());
   using Time = decltype(std::chrono::high_resolution_clock::now());
   // -------------------------------------------------------------------------------------
   leanstore::cr::CRManager::global->registerMeAsSpecialWorker();
   // -------------------------------------------------------------------------------------
   AsyncWriteBuffer async_write_buffer(ssd_fd, PAGE_SIZE, FLAGS_write_buffer_size);
   std::vector<BufferFrame*> cxl_cool_candidate_bfs, evict_to_ssd_candidate_bfs;
   // -------------------------------------------------------------------------------------
   auto next_cxl_bf_range = [&]() {
      const u64 BATCH_SIZE = FLAGS_replacement_chunk_size;
      cxl_cool_candidate_bfs.clear();
      for (u64 i = 0; i < BATCH_SIZE; i++) {
         BufferFrame* r_bf = &randomCXLBufferFrame();
         DO_NOT_OPTIMIZE(r_bf->header.state);
         cxl_cool_candidate_bfs.push_back(r_bf);
      }
   };
   // -------------------------------------------------------------------------------------
   while (bg_threads_keep_running) {
      // =================================================================================
      // Phase 1: CXL Cooling — HOT_CXL → COOL_CXL
      // =================================================================================
      [[maybe_unused]] Time phase_1_begin, phase_1_end;
      COUNTERS_BLOCK() { phase_1_begin = std::chrono::high_resolution_clock::now(); }
      volatile u64 failed_attempts = 0;
#define repickIf(cond)                       \
   if (cond) {                               \
      failed_attempts = failed_attempts + 1; \
      jumpmu_continue;                       \
   }
      auto& current_cxl_partition = *partitions[p_begin + utils::RandomGenerator::getRand<u64>(0, p_end - p_begin)];
      // Backoff: no eviction pressure and no pending eviction work → sleep to avoid busy-polling
      if (current_cxl_partition.cxl_free_list.counter >= current_cxl_partition.free_bfs_limit &&
          evict_to_ssd_candidate_bfs.empty()) {
         usleep(100);
         continue;
      }
      if ((current_cxl_partition.cxl_free_list.counter < current_cxl_partition.free_bfs_limit) && failed_attempts < 10) {
         next_cxl_bf_range();
         while (cxl_cool_candidate_bfs.size()) {
            jumpmuTry()
            {
               BufferFrame* r_buffer = cxl_cool_candidate_bfs.back();
               cxl_cool_candidate_bfs.pop_back();
               COUNTERS_BLOCK() { PPCounters::myCounters().phase_1_counter++; }
               // -------------------------------------------------------------------------------------
               BMOptimisticGuard r_guard(r_buffer->header.latch);
               repickIf(r_buffer->header.keep_in_memory || r_buffer->header.is_being_written_back || r_buffer->header.latch.isExclusivelyLatched());
               r_guard.recheck();
               // -------------------------------------------------------------------------------------
               if (r_buffer->header.state == BufferFrame::STATE::COOL) {
                  evict_to_ssd_candidate_bfs.push_back(reinterpret_cast<BufferFrame*>(r_buffer));
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
                  if (swip.isHOT()) {
                     BufferFrame* picked_child_bf = &swip.asBufferFrame();
                     r_guard.recheck();
                     if(isInCXL(picked_child_bf)){    // [Added]. Must stay in CXL Layer.
                        picked_a_child_instead = true;
                        cxl_cool_candidate_bfs.push_back(picked_child_bf);
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
               // Suitable CXL page found, cool it: HOT → COOL
               {
                  const PID pid = r_buffer->header.pid;
                  {
                     BMExclusiveUpgradeIfNeeded p_x_guard(parent_handler.parent_guard);
                     BMExclusiveGuard r_x_guard(r_guard);
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
      // Phase 2: CXL Eviction — COOL_CXL → SSD
      //   clean → evict directly
      //   dirty → async write to SSD, evict after completion (Phase 3)
      // =================================================================================
      FreedBfsBatch freed_cxl_bfs_batch;

      auto evict_cxl_bf = [&](BufferFrame& bf, BMOptimisticGuard& c_guard) {
         DTID dt_id = bf.page.dt_id;
         c_guard.recheck();
         ParentSwipHandler parent_handler = getDTRegistry().findParent(dt_id, bf);
         // -------------------------------------------------------------------------------------
         if (FLAGS_optimistic_parent_pointer) {
            if (parent_handler.is_bf_updated) {
               c_guard.guard.version += 2;
            }
         }
         // -------------------------------------------------------------------------------------
         paranoid(parent_handler.parent_guard.state == GUARD_STATE::OPTIMISTIC);
         BMExclusiveUpgradeIfNeeded p_x_guard(parent_handler.parent_guard);
         // Must use tryToExclusive() (non-blocking) instead of toExclusive() (blocking).
         // toExclusive() calls mutex.lock() which can block while already holding the
         // parent lock (p_x_guard), creating a lock-order deadlock with worker threads
         // that acquire child-then-parent in resolveSwip's isCOOL() path.
         c_guard.guard.tryToExclusive();
         // -------------------------------------------------------------------------------------
         if (FLAGS_crc_check && bf.header.crc) {
            ensure(utils::CRC(bf.page.dt, EFFECTIVE_PAGE_SIZE) == bf.header.crc);
         }
         // -------------------------------------------------------------------------------------
         ensure(!bf.isDirty());
         paranoid(!bf.header.is_being_written_back);
         paranoid(bf.header.state == BufferFrame::STATE::COOL);
         paranoid(parent_handler.swip.isCOOL());
         // -------------------------------------------------------------------------------------
         const PID evicted_pid = bf.header.pid;
         parent_handler.swip.evict(evicted_pid);
         // -------------------------------------------------------------------------------------
         // Reclaim CXL buffer frame
         bf.reset();
         // Release the exclusive latch via the guard's unlock() so that version
         // and mutex are updated atomically and consistently.  Do NOT call
         // fetch_add(LATCH_EXCLUSIVE_BIT) + mutex.unlock() here: toExclusive()
         // already incremented the version and locked the mutex; a second manual
         // fetch_add would flip the exclusive bit back to 0 prematurely, and the
         // subsequent guard destructor would then re-set the bit and double-unlock
         // the mutex (UB), leaving the latch permanently in the exclusive state.
         c_guard.guard.unlock();
         // -------------------------------------------------------------------------------------
         freed_cxl_bfs_batch.add(bf);
         // [Added]. [PullRequest]: The original logic is wrong, should accumulate util reach 128
         // That is why batch pushing works!
         if (freed_cxl_bfs_batch.size() >= std::min<u64>(FLAGS_worker_threads, 128)) {
            freed_cxl_bfs_batch.pushCXL(current_cxl_partition);
         }
         // -------------------------------------------------------------------------------------
         if (FLAGS_pid_tracing) {
            Tracing::mutex.lock();
            if (Tracing::ht.contains(evicted_pid)) {
               std::get<1>(Tracing::ht[evicted_pid])++;
            } else {
               Tracing::ht[evicted_pid] = {dt_id, 1};
            }
            Tracing::mutex.unlock();
         }
         // -------------------------------------------------------------------------------------
         COUNTERS_BLOCK() { PPCounters::myCounters().evicted_pages++; }
         diag.evictions.fetch_add(1, std::memory_order_relaxed);
      };
      // -------------------------------------------------------------------------------------
      for (volatile const auto& cooled_bf : evict_to_ssd_candidate_bfs) {
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
            if (cooled_bf->isDirty()) {
               // Dirty CXL page: must write to SSD first, then evict in Phase 3
               if (!async_write_buffer.full()) {
                  {
                     BMExclusiveGuard ex_guard(o_guard);
                     paranoid(!cooled_bf->header.is_being_written_back);
                     cooled_bf->header.is_being_written_back.store(true, std::memory_order_release);
                     if (FLAGS_crc_check) {
                        cooled_bf->header.crc = utils::CRC(cooled_bf->page.dt, EFFECTIVE_PAGE_SIZE);
                     }
                     PID wb_pid = cooled_bf_pid;
                     if (FLAGS_out_of_place) {
                        wb_pid = getPageID_AllocatorPartition(cooled_bf_pid).nextPID();
                     }
                     async_write_buffer.add(*cooled_bf, wb_pid);
                  }
               } else {
                  jumpmu_break;
               }
            } else {
               // Clean CXL page: evict directly
               evict_cxl_bf(*cooled_bf, o_guard);
            }
         }
         jumpmuCatch() {}
      }
      evict_to_ssd_candidate_bfs.clear();
      // =================================================================================
      // Phase 3: Handle async write completions — evict written-back clean CXL pages
      // =================================================================================
      if (async_write_buffer.submit()) {
         const u32 polled_events = async_write_buffer.pollEventsSync();
         async_write_buffer.getWrittenBfs(
             [&](BufferFrame& written_bf, u64 written_lsn, PID out_of_place_pid) {
                jumpmuTry()
                {
                   {
                      BMOptimisticGuard o_guard(written_bf.header.latch);
                      BMExclusiveGuard ex_guard(o_guard);
                      ensure(written_bf.header.is_being_written_back);
                      ensure(written_bf.header.last_written_plsn < written_lsn);
                      if (FLAGS_out_of_place) {
                         getPageID_AllocatorPartition(getPageID_AllocatorPartitionID(written_bf.header.pid)).freePageID(written_bf.header.pid);
                         written_bf.header.pid = out_of_place_pid;
                      }
                      written_bf.header.last_written_plsn = written_lsn;
                      written_bf.header.is_being_written_back = false;
                      PPCounters::myCounters().flushed_pages_counter++;
                   }
                }
                jumpmuCatch()
                {
                   written_bf.header.crc = 0;
                   written_bf.header.is_being_written_back.store(false, std::memory_order_release);
                }
                // After flush completes, if still COOL and now clean → evict CXL BF
                {
                   jumpmuTry()
                   {
                      BMOptimisticGuard o_guard(written_bf.header.latch);
                      if (written_bf.header.state == BufferFrame::STATE::COOL && !written_bf.header.is_being_written_back && !written_bf.isDirty()) {
                         evict_cxl_bf(written_bf, o_guard);
                      }
                   }
                   jumpmuCatch() {}
                }
             },
             polled_events);
      }
      if (freed_cxl_bfs_batch.size()) {
         freed_cxl_bfs_batch.pushCXL(current_cxl_partition);
      }
      COUNTERS_BLOCK() { PPCounters::myCounters().pp_thread_rounds++; }
   }
   bg_threads_counter--;
}
// -------------------------------------------------------------------------------------
}  // namespace storage
}  // namespace leanstore
