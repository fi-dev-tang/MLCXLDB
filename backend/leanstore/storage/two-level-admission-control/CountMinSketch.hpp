#pragma once
#include <cstdint>
#include <vector>
#include <mutex>
#include <atomic>
#include <limits>      // for std::numeric_limits
#include <algorithm>   // for std::min
#include <random>      // for std::mt19937_64
#include "Units.hpp"

// Explicit hash function for (page_id, slot_id)
namespace std{
    template<>
    struct hash<std::pair<uint64_t, uint16_t>>{
        size_t operator()(const std::pair<uint64_t, uint16_t>& p) const{
            size_t h1 = std::hash<uint64_t>{}(p.first);
            size_t h2 = std::hash<uint16_t>{}(p.second);
            return h1 ^ (h2 * 2654435761ULL);       // Knuth multiplicative hash
        }
    };
}

namespace leanstore::storage::two_level_admission_control{

//--------------------------------------------------------------------------
// Count-Min-Sketch(Supports multi-thread concurrent access.)
// Using PageID to represent page_id_count_min_sketch
// Using PageID and RecordID to represent page_id_record_id_count_min_sketch
// 
// cms_matrix[cms_row_num][cms_col_num]
// cms_matrix[hash_functions_number][...]
// cms_row_num: 12 - 16
// cms_col_num: Adjust according to the working set
// cms_matrix[cms_row_num][cms_col_num]
//                              col_0   col_1   col_2   col_3   col_4 ...
// row_0 (hash_function_0)    
// row_1 (hash_function_1)
// row_2 (hash_function_2)
// ...
// (hash_functions_num)
template<typename CountMinSketchKey>
class CountMinSketch{
protected:
    u64 cms_row_num;  
    u64 cms_col_num; 

    // using std::atomic<u64> to support lock-free Update and Lookup.
    std::unique_ptr<std::atomic<u64>[]> cms_matrix;    // matrix array

   // assert(cms_hash_functions.size() == cms_row_num);
    std::vector<u64> cms_hash_seeds;

    // Aging Mechanism needs mutex(All elements should havle)
    // Mutable: Allow this variable changed even in const function.
    mutable std::mutex cms_aging_mutex;

    // Visit macro at: row i, col j
    // Usage: cms_matrix[i][j] = at(i,j)
    std::atomic<u64>& at(u64 i, u64 j){
        return cms_matrix[i * cms_col_num + j];
    }
    const std::atomic<u64>& at(u64 i, u64 j) const {
        return cms_matrix[i * cms_col_num + j];
    }

    // Hash function
    u64 Hash(const CountMinSketchKey& key, u64 hash_index) const{
        std::hash<CountMinSketchKey> hasher;    // Create Hash Object.
        u64 hash_result = hasher(key);

        // Mix cms_hash_seeds and more complexed MurmurHash3 finalizer
        // instead of hash_result ^= cms_hash_seeds[hash_index];
        hash_result += cms_hash_seeds[hash_index];
        hash_result ^=(hash_result >> 33);
        hash_result *= 0xff51afd7e558ccdULL;
        hash_result ^=(hash_result >> 33);
        hash_result *= 0xc4ceb9fe1a85ec53ULL;
        hash_result ^=(hash_result >> 33);

        return hash_result;
    }

public:
    CountMinSketch(u64 cms_row_num, u64 cms_col_num): 
    cms_row_num(cms_row_num), 
    cms_col_num(cms_col_num), 
    cms_matrix(std::make_unique<std::atomic<u64>[]>(cms_row_num * cms_col_num)),
    cms_hash_seeds(cms_row_num){
        // Initialize Matrix (atomic cannot be copied, must use emplace_back)
        // std::atomic can not use copy or std::move, so avoid using std::vector's resize,
        // because resize will call copy or move constructor on std::atomic<u64>

        // Initialize different hash function seeds
        std::mt19937_64 rng(0x9e3779b9);      // Mersenne Twister 64-bit
        for(u64 i = 0; i < cms_row_num; i++){
            cms_hash_seeds[i] = rng();
        }
    }

    // Disable Copy and std::move
    CountMinSketch(const CountMinSketch&) = delete;
    CountMinSketch& operator=(const CountMinSketch&) = delete;
    CountMinSketch(CountMinSketch&&) = delete;
    CountMinSketch& operator=(CountMinSketch&&) = delete;

    // One Visit, trigger Update operation.
    void CMSUpdate(const CountMinSketchKey& key){
        for(u64 i = 0; i < cms_row_num; i++){
            u64 col_index = Hash(key, i) % cms_col_num;
            at(i, col_index).fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Query the corresponding value, minimal all the cms_hash_functions result.
    // if key is page_id, then usage: page_cms.CMSQuery(page_id);
    u64 CMSQuery(const CountMinSketchKey& key) const {
        u64 min_count = std::numeric_limits<u64>::max();

        for(u64 i = 0; i < cms_row_num; i++){
            u64 col_index = Hash(key, i) % cms_col_num;
            u64 count = at(i, col_index).load(std::memory_order_relaxed);
            // Traverse all possible result.
            min_count = std::min(min_count, count);
        }
        return min_count;
    }

    // Calling Aging every 1w requests
    // All the count halved, thread-safe
    void CMSAging(){
        std::lock_guard<std::mutex> lock(cms_aging_mutex);

        for(u64 i = 0; i < cms_row_num; i++){
            for(u64 j = 0; j < cms_col_num; j++){
                u64 old_value = at(i,j).load(std::memory_order_relaxed);
                at(i,j).store(old_value >> 1, std::memory_order_relaxed);  // Not strongly enforced CAS.
            }
        }
    }

    // Reset all counts, thread-safe
    void CMSReset(){
        std::lock_guard<std::mutex> lock(cms_aging_mutex);

        for(u64 i = 0; i < cms_row_num; i++){
            for(u64 j = 0; j < cms_col_num; j++){
                at(i,j).store(0, std::memory_order_relaxed);
            }
        }        
    }

    // Configuration parametes
    u64 CMSGetRowNum() const {return cms_row_num;}
    u64 CMSGetColNum() const {return cms_col_num;}

    size_t CMSGetMemoryUsage() const { return cms_row_num * cms_col_num * sizeof(std::atomic<u64>);}
};


//======================[Added]. Phase 1. Page-level-Admission=========================================
class PageCountMinSketch: public CountMinSketch<u64>{   // using page_id
public:
    explicit PageCountMinSketch(u64 cms_row_num = 12, u64 cms_col_num = 1024 * 16)
    : CountMinSketch<u64>(cms_row_num, cms_col_num){}

    void CMSPageAccessUpdate(u64 page_id){
        CMSUpdate(page_id);
    }

    u64 CMSGetPageAccessCount(u64 page_id) const {
        return CMSQuery(page_id);
    }
};

//======================[Added]. Phase 2. Record-level-Admission=========================================
class RecordCountMinSketch: public CountMinSketch<std::pair<u64, u16>>{   // using page_id
public:
    using CMSPageIDSlotIDKey = std::pair<u64, u16>;

    explicit RecordCountMinSketch(u64 cms_row_num = 12, u64 cms_col_num = 1024 * 8)
    : CountMinSketch<CMSPageIDSlotIDKey>(cms_row_num, cms_col_num){}

    void CMSRecordAccessUpdate(u64 page_id, u16 slot_id){
        CMSUpdate(std::make_pair(page_id, slot_id));
    }

    u64 CMSGetRecordAccessCount(u64 page_id, u16 slot_id) const {
        return CMSQuery(std::make_pair(page_id, slot_id));
    }
};
}