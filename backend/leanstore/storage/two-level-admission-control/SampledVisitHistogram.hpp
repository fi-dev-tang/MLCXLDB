#pragma once
#include"CountMinSketch.hpp"
#include"DepulicateSetForPageSampling.hpp"
#include"VisitCountBucket.hpp"
#include<atomic>
#include<mutex>
#include<string>
#include<iomanip>
#include<sstream>
#include "leanstore/Config.hpp"
//-----------------------------------[Added].-----------------------------------------------------
// [Caution]: Call this when (FLAGS_cxl_tiering_enabled == true).
// SampledVisitHistogram is used for:
// When DepulicateSetForPageSampling has reached 10000 requests, we build a SampledVisitHistogram
// 1. According to the 1024 different page_ids, we get the corresponding page_id_visit_count
// 2. We assign the page_id_visit_count to the VisitFrequency bucket
// 3. Then we build the SampledVisitHistogram, 
// The x-axis represents the range [2^n, 2^(n + 1) - 1], the y-axis represents the number of accesses.
// 4. read the configuartion parameters: 
// FLAGS_cxl_tiering_enabled, FLAGS_dram_buffer_pool_gib, FLAGS_cxl_gib
// Future we may consider adding FLAGS_dram_recordcache_gib for another consideration.
// (Helper Function): we can print the SampledVisitHistogram to monitor changes.
namespace leanstore::storage::two_level_admission_control{

// SampledVistHistorgam
// it manages: PageCountMinSketch, DepulicateSetForPageSampling, VisitFrequencyBucketArray
class SampledVisitHistogram{
private:
    // Using 3 core components
    PageCountMinSketch& page_cms;                   // Global Page access statistics
    DepulicateSetForPageSampling &sampling_set;     // default: Sampling Set for SampledVisitHistogram
    VisitFrequencyBucketArray& bucket_array;        // Frequency buckets, visit intervals in [2^n, 2^(n + 1) - 1].

    // Runtime state
    std::atomic<u64> current_admission_threshold_coarse;   // if a page's visit_count >= current_admission_threshold, can allow into DRAM Tier.
    std::atomic<u64> current_admission_threshold_fine;     // filter in the bucket to get a more accurate threshold.
   
    // Mutex for histogram update operations
    mutable std::mutex histogram_update_mutex;

public:
    // Constructor
    explicit SampledVisitHistogram(
        PageCountMinSketch& page_cms,
        DepulicateSetForPageSampling& sampling_set,
        VisitFrequencyBucketArray& bucket_array
    ): page_cms(page_cms),
    sampling_set(sampling_set),
    bucket_array(bucket_array),
    current_admission_threshold_coarse(1),
    current_admission_threshold_fine(1)
    {}

    //---------------------------------------------------------------------------------------------
    // Helper function:
    // Get DRAM-vs-CXL-total Ratio
    //---------------------------------------------------------------------------------------------
    double GetDRAMvsCXLRatio() const{
        double total_capacity = (FLAGS_cxl_gib + FLAGS_dram_recordcache_gib + FLAGS_dram_buffer_pool_gib) * 1.0;
        double dram_vs_cxl_total_ratio = static_cast<double>(FLAGS_dram_buffer_pool_gib + FLAGS_dram_recordcache_gib) / static_cast<double>(total_capacity);
        return dram_vs_cxl_total_ratio;
    }

     //---------------------------------------------------------------------------------------------
    // Helper function:
    // Get Total Sampled Pages
    //---------------------------------------------------------------------------------------------
    u64 GetTotalSampledPages() const{
        u64 total_sampled_pages = 0;
        s64 bucket_num = bucket_array.GetBucketsNum();

        for(s64 i = 0; i < bucket_num; i++){
            total_sampled_pages += bucket_array.GetTargetBucket(i).GetBucketCount();
        }
        return total_sampled_pages;
    }

    // Core function
    //---------------------------------------------------------------------------------------------
    // CalculateAdmissionThreshold: Calculate the admission threshold based on DRAM: CXL ratio
    // Algorithm:
    // 1. Calculate DRAM_buffer_pool page capcity
    // 2. Calculate target page count = total_pages_number * dram/ CXL ratio
    // 3. Accumulate from high-frequency buckets to low-frequency buckets
    // 4. find the bucket where cumulative count >= target_page_count
    // Return the lower bound of that bucket(2^n) as the threshold
    // For example:
    // PAGE_SIZE = 16 KB, FLAGS_dram_buffer_pool_gib = 4GB and FLAGS_cxl_gib = 32GB
    // DRAM / (DRAM + CXL) ratio: 11.1%
    // Try to find the most frequently visited 11.1% pages correspond visit_count.
    // If we have 1024 pages, then we find the top 1024 * 11.1% = 114 pages --> visit_count
    u64 CalculateAdmissionThreshold_coarse() const{
        // 1. calcualte total sampled pages, may not equal to 1024
        // because we use aging, there have remaining old pages
        // we calcualte all the bucket_array, instead of query depulicate_sets.size()
        u64 total_sampled_pages = 0;
        s64 bucket_num = bucket_array.GetBucketsNum();

        for(s64 i = 0; i < bucket_num; i++){
            total_sampled_pages += bucket_array.GetTargetBucket(i).GetBucketCount();
        }

        // If no pages sampled, return minimum treshold
        if(total_sampled_pages == 0){
            return 1;
        }

        // 2. Calculate target page count based on DRAM ratio
        double dram_vs_cxl_total_ratio = GetDRAMvsCXLRatio();

        // if total_sampled_pages = 1024, and dram_vs_cxl_total_ratio = 11.1%
        // Then we have 114 pages as cumulative pages.
        u64 target_page_count = static_cast<u64> (total_sampled_pages * dram_vs_cxl_total_ratio);


        // 4. Accumulate from high-frequency buckets to low-frequency buckets
        u64 cumulative_count = 0;
        for(s64 i = bucket_num - 1; i >= 0; i--){
            cumulative_count += bucket_array.GetTargetBucket(i).GetBucketCount();

            if(cumulative_count >= target_page_count){
                return bucket_array.GetTargetBucket(i).bucket_range_lower_bound;
            }
        } 
        return bucket_array.GetTargetBucket(0).bucket_range_lower_bound;
    }

    // Core function(fine_grained version)
    // Same as CalculateAdmissionThreshold_coarse, but more fine_grained
    // When invoking CalculateAdmissionThreshold_coarse and targeting a specific bucket x,
    // we backtrack to the depulicate_sets to retrieve the access counts for page_id s
    // falling within the range [2^x, 2^(x + 1) - 1], 
    // enabling a finer-grained partitioning.
    u64 CalculateAdmissionThreshold_fine(const std::vector<std::pair<u64, u64>>& sampled_snapshot){
        // 1. calculate total_sampled_pages
        u64 total_sampled_pages = 0;
        s64 bucket_num = bucket_array.GetBucketsNum();

        for(s64 i = 0; i < bucket_num; i++){
            total_sampled_pages += bucket_array.GetTargetBucket(i).GetBucketCount();
        }
        if(total_sampled_pages == 0){
            return 1;
        }

        // 2. Calculate target page count based on DRAM ratio
        double dram_vs_cxl_total_ratio = GetDRAMvsCXLRatio();

        // if total_sampled_pages = 1024, ratio = 11.1%, then target_page_count = 114 pages.
        u64 target_page_count = static_cast<u64>(total_sampled_pages * dram_vs_cxl_total_ratio);

        // 3. fine-grained allowed, narrow [2^x, 2^(x + 1) - 1] to [critical_lower, critical_upper]
        u64 cumulative_before_critical = 0;
        s64 critical_bucket_index = -1;     // -1 means not found.

        for(s64 i = bucket_num - 1; i >= 0; i--){
            u64 bucket_count = bucket_array.GetTargetBucket(i).GetBucketCount();
            if(cumulative_before_critical + bucket_count >= target_page_count){
                critical_bucket_index = i;
                break;
            }
            cumulative_before_critical += bucket_count;
        }

        if(critical_bucket_index == -1){
            return bucket_array.GetTargetBucket(0).bucket_range_lower_bound;
        }

        u64 critical_lower = bucket_array.GetTargetBucket(critical_bucket_index).bucket_range_lower_bound;
        u64 critical_upper = bucket_array.GetTargetBucket(critical_bucket_index).bucket_range_upper_bound;

        //------------------------------[Added].-------------------------------------------------------------
        // Fine-grained refinement with [critical_lower, critical_upper]
        // remaining_needed_pages and the sorted depulicate_sets in [critical_lower, critical_upper]
        u64 remaining_needed_pages = target_page_count - cumulative_before_critical;

        // Filter: only pages in [critical_lower, critical_upper]
        std::vector<u64> visit_count_in_ranges;
        visit_count_in_ranges.reserve(sampled_snapshot.size());

        for(auto& [page_id, page_id_corresponding_visit_count]: sampled_snapshot){
            if(page_id_corresponding_visit_count >= critical_lower && page_id_corresponding_visit_count <= critical_upper){
                visit_count_in_ranges.push_back(page_id_corresponding_visit_count);
            }
        }

        // Corner-case: visit_count_in_ranges is empty.
        if(visit_count_in_ranges.empty()){
            return critical_lower;
        }

        // Sort in descending order, because we want the top [remaining_needed_pages]
        // For example,
        // Before sort: [45, 33, 61, 38, 55, 34]
        // After sort:  [61, 55, 45, 38, 33, 34]
        //  if remaining_needed_pages = 3, then chose 38 as threshold.
        std::sort(visit_count_in_ranges.begin(), visit_count_in_ranges.end(),std::greater<u64>());

        // 
        // For example:
        // critical bucket: [32, 63]
        // cumulative_before_critical = 103
        // remaining_needed_pages = 114 - 103 = 11
        // while visit_count_in_ranges:
        // [61, 59, 57, 55, 52, 49, 47, 45, 43, 40, 38, 36, 34, 32]
        // remaining_needed = 11, threshold = 38
        // Meaning: pages with visit_count >= 38 should enter DRAM Tier
        if(remaining_needed_pages >= visit_count_in_ranges.size()){
            return critical_lower;
        }

        // The threshold is the visit count at position [remaining_needed_pages - 1]
        u64 fine_threshold = visit_count_in_ranges[remaining_needed_pages - 1];
        return fine_threshold;
    }


    // UpdateSampledVisitHistogram
    //---------------------------------------------------------------------------------------------
    // UpdateSampledVisitHistorgram: Build the histogram and calculate admission threshold.
    // This should be called when DepulicateSetForPageSampling reaches 10000 requests.
    //---------------------------------------------------------------------------------------------
    void UpdateSampledVisitHistogram(){
        std::lock_guard<std::mutex> lock(histogram_update_mutex);

        // 1. Write Operation on depulicate_sets
        // Fill sampled page visit counts from PageCountMinSketch
        // After calling FillSampledPageVisitCounts, 
        // we now have std::unordered_map<page_id, page_id_corresponding_visit_count>
        sampling_set.FillSampledPageVisitCounts(page_cms);  
    
        // 2. Read Operation on depulicate_sets
        // First sample then Aging
        auto sampling_set_result = sampling_set.GetSampledVisitCountSnapshot();

        // 3. Aging Mechanism for previous bucket counts(prepare for new histogram)
        // Future: Here we can also try ResetAll
        // Aging is called before fill data into depulicate_sets
        bucket_array.AgingVisitFrequencyBuckets();
        page_cms.CMSAging();

        // 4. Build historgram by Updaing buckets
        for(const auto &[page_id, page_id_corresponding_visit_count]: sampling_set_result){
            if(page_id_corresponding_visit_count > 0){
                bucket_array.UpdateVisitFrequencyBucket(page_id_corresponding_visit_count);
            }
        }

        // 5. Calculate admission threshold based on DRAM: CXL ratio
        u64 new_threshold_coarse = CalculateAdmissionThreshold_coarse();
        u64 new_threshold_fined = CalculateAdmissionThreshold_fine(sampling_set_result);

        current_admission_threshold_coarse.store(new_threshold_coarse, std::memory_order_relaxed);
        current_admission_threshold_fine.store(new_threshold_fined, std::memory_order_relaxed);

        // 6. Reset sampling set for next round, and call bucket_array's aging, and page_cms's aging
        sampling_set.Reset();
    }


    //-------------------------------------------------------------------------------------------
    // ShouldAdmitToDRAM: Check if a page should be admitted to DRAM based on its visit count
    //--------------------------------------------------------------------------------------------
    bool ShouldAdmitToDRAM(u64 page_id) const{
        if(!FLAGS_cxl_tiering_enabled){
            return true;    // bypass
        }
        u64 page_visit_count = page_cms.CMSGetPageAccessCount(page_id);
        u64 threshold_coarse = current_admission_threshold_coarse.load(std::memory_order_relaxed);

        return page_visit_count >= threshold_coarse;
    }

    // GetAdmissionThreshold
    u64 GetAdmissionThreshold_coarse() const{
        return current_admission_threshold_coarse.load(std::memory_order_relaxed);
    }

    u64 GetAdmissionThreshold_fine() const {
        return current_admission_threshold_fine.load(std::memory_order_relaxed);
    }

    bool ShouldTriggerUpdate() const {
        return sampling_set.ShouldTriggerVisitHistogramUpdate();
    }

    //---------------------------------------------------------------------------------------------
    // Multi-threaded Design: Separate Worker Threads and Background Thread
    //---------------------------------------------------------------------------------------------
    // Concurrent Issue (if using single OnPageAccess in multi-thread):
    // Thread A: OnPageAccess(page_id = 5)
    // Thread B: OnPageAccess(page_id = 7)  
    //                 t1               t2                  t3                          t4
    // Thread A:  Update CMS   -> Update Sampling_set  -> ShouldUpdate = true  ->   Update(Acquire lock, Reset samling_set, release Lock)
    // Thread B:  Update CMS   -> Update Sampling_set  -> ShouldUpdate = true(wait for lock)                                                -> Acquire lock, sampling_set is empty.
    // 
    // Problem: if Thread A check shoudUpdate = true, and Thread B check shouldUpdate = true
    // Thread A reset, then Thread B enters, use the empty sampling_set to update visit histogram, then threshold == 1
    //
    // Solution: Separate responsibilities
    // 1. Worker threads: Only responsible for counting (Update CMS + Update Sampling_set)
    // 2. Background thread: Only responsible for checking and triggering histogram update
    //---------------------------------------------------------------------------------------------

    //---------------------------------------------------------------------------------------------
    // WorkerThreadOnPageAccess: Called by worker threads (critical path)
    // Only update counters, DO NOT trigger histogram update
    //---------------------------------------------------------------------------------------------
    void WorkerThreadOnPageAccess(u64 page_id){
        // 1. Update CMS
        page_cms.CMSPageAccessUpdate(page_id);

        // 2. Update sampling_set
        sampling_set.OnPageAccess(page_id);

        // Worker threads do NOT check or trigger histogram update
    }

    //---------------------------------------------------------------------------------------------
    // BackgroundThreadTryUpdate: Called by a single background thread
    // Check if update is needed and trigger histogram update
    // Returns: true if update was performed, false otherwise
    //---------------------------------------------------------------------------------------------
    bool BackgroundThreadTryUpdate(){
        if(sampling_set.ShouldTriggerVisitHistogramUpdate()){
            UpdateSampledVisitHistogram();
            return true;
        }
        return false;
    }

    //---------------------------------------------------------------------------------------------
    // OnPageAccess: Single-threaded version (for backward compatibility or simple scenarios)
    // Use WorkerThreadOnPageAccess + BackgroundThreadTryUpdate in multi-threaded scenarios
    //---------------------------------------------------------------------------------------------
    void OnPageAccess(u64 page_id){
        // 1. Update CMS
        page_cms.CMSPageAccessUpdate(page_id);

        // 2. Update depulicate_sets
        sampling_set.OnPageAccess(page_id);

        // 3. Check whether should Update VisitHistogram
        if(sampling_set.ShouldTriggerVisitHistogramUpdate()){
            UpdateSampledVisitHistogram();
        }
    }

    //-----------------[Added.] Printing Part--------------------------------------------------------
    //-------------------------------------------------------------------------------------
    // PrintHistogram: Print the histogram for monitoring and debugging
    // Format:
    // ========== Sampled Page Visit Histogram ==========
    // DRAM:CXL Ratio: 1:2 (DRAM: 16 GiB, CXL: 32 GiB)
    // Current Admission Threshold: 20 visits
    // Total Updates: 15
    // Last Update: 150000 requests
    // 
    // Bucket | Visit Range        | Page Count | Cumulative | Percentage | Bar
    // -------|--------------------|-----------:|------------|------------|----
    //   0    | [1]                |        150 |        150 |      14.7% | ███████
    //   1    | [2, 3]             |        200 |        350 |      19.6% | ██████████
    //   2    | [4, 7]             |        250 |        600 |      24.4% | ████████████
    //  ...
    //-------------------------------------------------------------------------------------
    std::string PrintHistogram() const {
        std::ostringstream oss;

        oss << "\n========== Sampled Page Visit Histogram ==========\n";
        double cxl_ratio = static_cast<double>(FLAGS_cxl_gib) / FLAGS_dram_buffer_pool_gib;
        oss << "DRAM: CXL Ratio: 1:" << std::fixed << std::setprecision(1) << cxl_ratio << "\n";
        oss << "Current Admission Threshold Coarse: " << current_admission_threshold_coarse.load(std::memory_order_relaxed) << " visits\n";
        oss << "Current Admission Threshold Fined: " << current_admission_threshold_fine.load(std::memory_order_relaxed) << " visits\n";
        oss << "Total Sampled Pages: " << GetTotalSampledPages() << "\n";
        oss << "\n\n";

        // Print table header
        oss << std::left << std::setw(7) << "Bucket" << " | "
            << std::setw(20) << "Visit Range" << " | "
            << std::right << std::setw(11) << "Page Count" << " | "
            << std::setw(11) << "Cumulative" << " | "
            << std::setw(10) << "Percentage" << " | "
            << "Bar\n";
        oss << std::string(7, '-') << "-+-"
            << std::string(20, '-') << "-+-"
            << std::string(11, '-') << "-+-"
            << std::string(11, '-') << "-+-"
            << std::string(10, '-') << "-+-"
            << std::string(20, '-') << "\n";

        // Print each bucket
        u64 cumulative = 0;
        for(s64 i = 0; i < bucket_array.GetBucketsNum(); i++){
            u64 count = bucket_array.GetTargetBucket(i).GetBucketCount();
            cumulative += count;

            const auto& bucket = bucket_array.GetTargetBucket(i);
            
            // Format visit range
            std::ostringstream range_oss;
            if(i == 0){
                range_oss << "[1]";
            } else {
                range_oss << "[" << bucket.bucket_range_lower_bound << ", " 
                         << bucket.bucket_range_upper_bound << "]";
            }

            // Calculate percentage
            double percentage = GetTotalSampledPages() > 0 ?
                100.0 * count /  GetTotalSampledPages() : 0.0;

            // Create bar (max 50 chars)
            int bar_length =  GetTotalSampledPages() > 0 ?
                static_cast<int>(50.0 * count / GetTotalSampledPages()) : 0;
            std::string bar;
            bar.reserve(bar_length * 3);  // '█' in UTF-8 use 3 Byte
            for(int j = 0; j < bar_length; j++){
                bar += "█";               // Use string concat
            }

            // Mark threshold
            std::string threshold_marker = "";
            if(bucket.bucket_range_lower_bound == current_admission_threshold_coarse.load(std::memory_order_relaxed)){
                threshold_marker = " <- THRESHOLD";
            }

            oss << std::right << std::setw(6) << i << " | "
                << std::left << std::setw(20) << range_oss.str() << " | "
                << std::right << std::setw(11) << count << " | "
                << std::setw(11) << cumulative << " | "
                << std::setw(9) << std::fixed << std::setprecision(1) << percentage << "% | "
                << bar << threshold_marker << "\n";
        }

        oss << "==================================================\n";

        return oss.str();
    }
};
}

//---------------------------------------[Added].----------------------------------------------------------
//
// SampledVisitHistogram module exposes a single public interface: 
// OnPageAccess
// When a request for a specific page_id arrives, 
// 1. system updates the visit_count in PageCountMinSketch associated with the page_id
// 2. it processes the sampling_set by invoking its own OnPageAccess method,
// if condition meets (1w) to update the access frequency histogram, the core function UpdateSampledVisitHistogram() is triggered.
// 3. In UpdateSampledVisitHistogram()
//  (a). Fullfill the sampling_set
//  (b). Aging the PageCountMinSketch and VisitFrequencyBuckets
//  (c). Check the current_admission_threshold_coarse and current_admission_threshold_fine fields
//  (d). resets the results within the sampling_set.
//
// OnPageAccess(page_id) -> Update PageCountMinSketch
// -> Update depulicate_sets -> (Check if it up to 10000 requests)
// -> trigger UpdateSampledVisitHistogram()
// -> fill sampling_set -> Aging for PageCountMinSketch and buckets -> Update Visit Histogram -> calculate threshold(coarse/fine) -> Reset Sampling Set.