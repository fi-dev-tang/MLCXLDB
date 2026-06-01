#pragma once
#include"Units.hpp"
#include"CountMinSketch.hpp"
#include<unordered_map>
#include<mutex>
#include<vector>
#include<atomic>

//-----------------------[Added].-----------------------------------------------------
// DepulicateSetForPageSampling:
// Usage:
// 1. First, it stores FLAGS_max_sampled_page_ids distinct pages, (without read their page_visit_count)
// 2. When in Hot-Path, the total visit achieve FLAGS_trigger_visit_histogram_update_size times, than we start reading
// 
// request in 1 ~ FLAGS_trigger_visit_histogram_update_size:
// On-every-page-visit:
// 1. The formal PageCountMinSketch update corresponding page_id's visit_count
// 2. if DepulicateSetForPageSampling.size() < FLAGS_max_sampled_page_ids, insert the page_id
// (only insert page_id, without read the PageCMS)
//
// request = FLAGS_trigger_visit_histogram_update_size:
// We can update the SampledVisitHistogram
// traverse set's FLAGS_max_sampled_page_ids different page_id:
// 1. visit_count = CMS.Query(page_id)
// 2. bucket_array.Update(visit_count)
//
// After successfully build SampledVisitHistogram, empty the set and enter the next round.
// 
// [Trigger Condition]
// Update SampledVisitHistogram when request count reaches FLAGS_trigger_visit_histogram_update_size.
// Corner case: If trigger window requests don't generate FLAGS_max_sampled_page_ids distinct pages,
// we still trigger update with whatever pages we've sampled (could be < FLAGS_max_sampled_page_ids).
namespace leanstore::storage::two_level_admission_control{

class DepulicateSetForPageSampling{
private:
    // Configurable parameters
    u64 max_sampled_page_ids;          // at-most 1024 pages.
    u64 trigger_visit_histogram_update_size;         // 1w requests

    // Runtime parameters.
    // std::unordered_map<page_id, page_id's_corresponding_visit_count>;
    std::unordered_map<u64, u64> depulicate_sets;
    alignas(64) std::atomic<u64> current_request_count;     // current request count in this window 

    // Mutex for thread-safe operation on the set
    mutable std::mutex depulicate_sets_mutex;

public:
    // DepulicateSetForPageSampling constructor
    explicit DepulicateSetForPageSampling(
    u64 max_sampled_page_ids = 1024,
    u64 trigger_visit_histogram_update_size = 10000                                    
    ): max_sampled_page_ids(max_sampled_page_ids),
       trigger_visit_histogram_update_size(trigger_visit_histogram_update_size),
       current_request_count(0)
    {
        depulicate_sets.reserve(max_sampled_page_ids); // pre-allocate
    }

    //-----------------------------[Added].---------------------------------------------
    // OnPageAccess:
    // 1. add to current_request_count(atomic)
    // 2. when current_request_count < 10000 && depulicate_sets.size() < 1024, 
    // push page_id into depulicate_sets.
    // because it is on critical path, 
    // so we use try_lock()
    void OnPageAccess(u64 page_id){
        current_request_count.fetch_add(1, std::memory_order_relaxed);

        // On critical path, not strongly enforce mutex.lock()
        if(depulicate_sets_mutex.try_lock()){
            // Double-check:
            if(depulicate_sets.size() < max_sampled_page_ids){
                depulicate_sets.emplace(page_id, 0);
            }
            depulicate_sets_mutex.unlock();
        }
    }

    //-----------------------------[Added].---------------------------------------------
    // ShouldTriggerVisitHistogramUpdate(check if >= 10000)
    bool ShouldTriggerVisitHistogramUpdate() const{
        u64 req_count = current_request_count.load(std::memory_order_relaxed);
        return req_count >= trigger_visit_histogram_update_size;
    }

    // Write Operation for depulicate_sets
    void FillSampledPageVisitCounts(const PageCountMinSketch& page_cms){
        std::lock_guard<std::mutex> lock(depulicate_sets_mutex);

        for(auto & [page_id, page_id_correspond_visit_count]: depulicate_sets){
            page_id_correspond_visit_count = page_cms.CMSQuery(page_id);
        }
    }

    // Calling interface for GetSampledVisitCountSnapshot
    // Read Operation for depulicate_sets
    std::vector<std::pair<u64, u64>> GetSampledVisitCountSnapshot() const{
        std::lock_guard<std::mutex> lock(depulicate_sets_mutex);
        return {depulicate_sets.begin(), depulicate_sets.end()};
    }

    u64 GetCurrentRequestCount() const {
        return current_request_count.load(std::memory_order_relaxed);
    }

    // Check the number of distinct page_ids sampled so far
    u64 GetSampledPageCount() const{
        std::lock_guard<std::mutex> lock(depulicate_sets_mutex);
        return depulicate_sets.size();
    }

    // Reset, empty the sampling set and reset counters for next round
    void Reset(){
        std::lock_guard<std::mutex> lock(depulicate_sets_mutex);
        depulicate_sets.clear();
        current_request_count.store(0, std::memory_order_relaxed);
    }

    //--------------------------------------------------------------------
    // Configuration parameters(Get & Set)
    //--------------------------------------------------------------------
    u64 GetMaxSampledPageIds() const { return max_sampled_page_ids;}
    u64 GetTriggerVisitHistogramWindow() const {return trigger_visit_histogram_update_size;}
    
    void SetMaxSampledPageIds(u64 size){
        std::lock_guard<std::mutex> lock(depulicate_sets_mutex);
        max_sampled_page_ids = size;
    }
    void SetTriggerVisitHistogramWindow(u64 size){
        trigger_visit_histogram_update_size = size;
    }

    //--------------------------------------------------------------------
    // Monitoring Snapshot for Statistics
    //--------------------------------------------------------------------
    struct DepulicateSetForPageSamplingSnapShot{
        u64 current_request_count;
        u64 sampled_page_ids;
        u64 max_sampled_page_ids;           // default: 1024
        u64 trigger_visit_histogram_update_size;
        double sampling_progress;           // 0.0 ~ 1.0
        double sample_fill_ratio;           // sampled_page_ids / max_sampled_page_ids;
    };

    DepulicateSetForPageSamplingSnapShot GetStatistics() const{
        std::lock_guard<std::mutex> lock(depulicate_sets_mutex);

        u64 req_count = current_request_count.load(std::memory_order_relaxed);
        u64 sampled_count = depulicate_sets.size();

        return DepulicateSetForPageSamplingSnapShot{
            .current_request_count = req_count,
            .sampled_page_ids = sampled_count,
            .max_sampled_page_ids = max_sampled_page_ids,
            .trigger_visit_histogram_update_size = trigger_visit_histogram_update_size,
            .sampling_progress = trigger_visit_histogram_update_size > 0?
            static_cast<double>(req_count) / trigger_visit_histogram_update_size : 0.0,
            .sample_fill_ratio = max_sampled_page_ids > 0?
            static_cast<double>(sampled_count) /  max_sampled_page_ids: 0.0
        };
    }
};
}