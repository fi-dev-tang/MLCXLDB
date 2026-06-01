#pragma once

#include<atomic>
#include<vector>
#include<mutex>
#include"Units.hpp"
//----------------------------[Added].---------------------------------------------------
// These buckets create hot page admission threshold.
// VisitCountBucket is used for the first level admission control, in page-granularity.
// 
// 1. Buckets records visit count range in [2^n, 2^(n + 1) - 1]'s page number to cater to zipfian distribution
// 2. Support configurable bucket numbers
// 3. Aging mechanism for halve the content
// 4. Determine the admission capacity tthreshold based on the DRAM: CXL ratio.
// Example:
// Bucket_0: [2^0, 2^1 - 1] = [1]
// Bucket_1: [2^1, 2^2 - 1] = [2, 3]
// Bucket_2: [2^2, 2^3 - 1] = [4, 7]
// Bucket_3: [2^3, 2^4 - 1] = [8, 15]
// ....
// Bucket_13: [2^13, 2^14 -1] = [8192, 16383] <- 10000
// Bucket_14: [2^14, 2^15 -1] = [16384, 32767]
namespace leanstore::storage::two_level_admission_control{
class VisitFrequencyBucket{                     // Record page visit count in range [2^n, 2^(n + 1) - 1].
public:
    u64 bucket_index;                           // represent the [2^n, 2^(n + 1) - 1]
    u64 bucket_range_lower_bound;               // 2^n
    u64 bucket_range_upper_bound;               // 2^(n + 1) - 1
    std::atomic<u64> page_in_range_count;       // Page count in this range

    explicit VisitFrequencyBucket(u64 index):
    bucket_index(index),
    bucket_range_lower_bound(1ULL << bucket_index),
    bucket_range_upper_bound((1ULL << (bucket_index + 1)) - 1),
    page_in_range_count(0){}

    // The following support for move constructor and move function is only used to support emplace_back.
    // Move Constructor
    VisitFrequencyBucket(VisitFrequencyBucket&& other) noexcept
    : bucket_index(other.bucket_index),
    bucket_range_lower_bound(other.bucket_range_lower_bound),
    bucket_range_upper_bound(other.bucket_range_upper_bound),
    page_in_range_count(other.page_in_range_count.load(std::memory_order_relaxed)){}

    // Copy Constructor
    VisitFrequencyBucket(const VisitFrequencyBucket& other) noexcept
    : bucket_index(other.bucket_index),
    bucket_range_lower_bound(other.bucket_range_lower_bound),
    bucket_range_upper_bound(other.bucket_range_upper_bound),
    page_in_range_count(other.page_in_range_count.load(std::memory_order_relaxed)){}

    // Disable assignment(move assignment and copy assignment)
    VisitFrequencyBucket& operator=(const VisitFrequencyBucket&) = delete;
    VisitFrequencyBucket& operator=(VisitFrequencyBucket&&) = delete;

    
    void Accumulate_the_bucket(){
        page_in_range_count.fetch_add(1, std::memory_order_relaxed);
    }

    u64 GetBucketCount() const {
        return page_in_range_count.load(std::memory_order_relaxed);
    }

    void SetBucketCount(u64 count){
        page_in_range_count.store(count, std::memory_order_relaxed);
    }

    void Reset(){
        page_in_range_count.store(0, std::memory_order_relaxed);
    }
};


//------------------------[Added].----------------------------------------------
// VisitFrequencyBucketArray : Manage the VisitFrequencyBucket
// Manage multiple VisitFrequencyBucket, to create visitHistogram.
class VisitFrequencyBucketArray{
private:
    u64 total_bucket_num;                               // Number of buckets (16 - 16 - more, configurable)

    std::vector<VisitFrequencyBucket> buckets;                        // Array of buckets
    mutable std::mutex aging_mutex;                     // Mutex for Aging operation

public:
    explicit VisitFrequencyBucketArray(u64 bucket_nums = 16)
    : total_bucket_num(bucket_nums){
        buckets.reserve(total_bucket_num);      // avoid using resize when handling std::atomic<u64>
        for(u64 i = 0; i < total_bucket_num; i++){
            buckets.emplace_back(i);            // In-place initialize.
        }
    }

    // get total_bucket_num
    u64 GetBucketsNum() const {
        return total_bucket_num;
    }

    // get the targeted indexed VisitFrequencyBucket (const version).
    const VisitFrequencyBucket& GetTargetBucket(u64 index) const {
        return buckets[index];
    }

    // get the targeted indexed VisitFrequencyBucket (not const version).
    VisitFrequencyBucket& GetTargetBucket(u64 index){
        return buckets[index];
    }

    //------------------------------------------------------------------------
    // CalculateBucketIndex: calculate one_page_visit_count's bucket index
    u64 CalculateBucketIndex(u64 one_page_visit_count) const{
        assert(one_page_visit_count > 0);               // Wrong, because we do not count visit_count = 0 's case.

        if(one_page_visit_count == 1) { return 0;}

        // using __builtin_clzll to calculate the formal 0, get log2 quickly
        // 63 - clz(x) = floor(log2(x))
        u64 bucket_index = 63 - __builtin_clzll(one_page_visit_count);

        if(bucket_index >= total_bucket_num){
            bucket_index = total_bucket_num - 1;
            // [Futuer]. If the bucket is not enough, we change this combination operation to dynamic resize.
        }
        return bucket_index;
    }

    //-------------------------------------------------------------------------
    // UpdateVisitFrequencyBucket:
    // Updates the corresponding Bucket based on the visit count.
    // visit_count: the number of one page's visit count.
    //
    // Algorithm: Calculate log2(visit_count) to determin which bucket to update.
    // Page with visit counts in range [2^n, 2^(n + 1) - 1] are assigned to Bucket_n.
    void UpdateVisitFrequencyBucket(u64 one_page_visit_count){
        assert(one_page_visit_count > 0);             // Wrong, we do not count visit_count = 0's case
        
        u64 bucket_index = CalculateBucketIndex(one_page_visit_count);
        buckets[bucket_index].Accumulate_the_bucket();
    }

    //----------------------------------------------------------------------------------------------
    // GetCumulativeCount: start from the begin_bucket_index to end's cumulative page visit count
    // 
    // Check the page's higher than start threshold's total page visit count
    u64 GetCumulativePageCount(u64 begin_bucket_index) const{
        u64 cumulative_page_count = 0;
        for(u64 i = begin_bucket_index; i < total_bucket_num; i++){
            cumulative_page_count += buckets[i].GetBucketCount();
        }
        return cumulative_page_count;
    }

    //----------------------------------------------------------------------------------------------
    // Aging Mechanism.
    //---------------------------------[Added.]--------------------------------------------------
    // Aging Logic here.
    // For example:(Suppose we only have 4 buckets, before Aging)
    //          Bucket index:       [0]         [1]         [2]         [3]         [4]
    // Page_visit_count_range:      [1]        [2,3]       [4,7]       [8,15]      [16, 31]
    //                 counts:      50          100         200         300         400

    // (after Aging), shift operation:
    //          Bucket index:       [0]         [1]         [2]         [3]         [4]
    // Page_visit_count_range:      [1]        [2,3]       [4,7]       [8,15]      [16, 31]
    //                 counts:      100         200         300         400           0
    void AgingVisitFrequencyBuckets(){
        std::lock_guard<std::mutex> lock(aging_mutex);

        // Array shift
        for(u64 i = 0; i < total_bucket_num - 1; i++){
            u64 next_count = buckets[i + 1].GetBucketCount();
            buckets[i].SetBucketCount(next_count);
        }
        buckets[total_bucket_num - 1].Reset();
    }

    void ResetAll(){
        std::lock_guard<std::mutex> lock(aging_mutex);
        for(auto& bucket: buckets){
            bucket.Reset();
        }
    }
};
}