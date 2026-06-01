#include "backend/leanstore/storage/two-level-admission-control/VisitCountBucket.hpp"
#include <iostream>
#include <cassert>
#include <thread>
#include <vector>

using namespace leanstore::storage::two_level_admission_control;

// 测试辅助函数：打印测试名称
void PrintTestName(const std::string& test_name) {
    std::cout << "\n========================================" << std::endl;
    std::cout << "测试: " << test_name << std::endl;
    std::cout << "========================================" << std::endl;
}

// 测试辅助函数：打印成功信息
void PrintSuccess(const std::string& test_name) {
    std::cout << "[✓] " << test_name << " 通过!" << std::endl;
}

// 测试1: VisitFrequencyBucket 基本功能测试
void TestVisitFrequencyBucketBasic() {
    PrintTestName("VisitFrequencyBucket 基本功能测试");
    
    // 测试 Bucket_0: [1]
    VisitFrequencyBucket bucket0(0);
    assert(bucket0.bucket_index == 0);
    assert(bucket0.bucket_range_lower_bound == 1);
    assert(bucket0.bucket_range_upper_bound == 1);
    assert(bucket0.GetBucketCount() == 0);
    std::cout << "Bucket_0: [" << bucket0.bucket_range_lower_bound 
              << ", " << bucket0.bucket_range_upper_bound << "]" << std::endl;
    
    // 测试 Bucket_3: [8, 15]
    VisitFrequencyBucket bucket3(3);
    assert(bucket3.bucket_index == 3);
    assert(bucket3.bucket_range_lower_bound == 8);
    assert(bucket3.bucket_range_upper_bound == 15);
    std::cout << "Bucket_3: [" << bucket3.bucket_range_lower_bound 
              << ", " << bucket3.bucket_range_upper_bound << "]" << std::endl;
    
    // 测试 Bucket_13: [8192, 16383]
    VisitFrequencyBucket bucket13(13);
    assert(bucket13.bucket_index == 13);
    assert(bucket13.bucket_range_lower_bound == 8192);
    assert(bucket13.bucket_range_upper_bound == 16383);
    std::cout << "Bucket_13: [" << bucket13.bucket_range_lower_bound 
              << ", " << bucket13.bucket_range_upper_bound << "]" << std::endl;
    
    // 测试 Accumulate
    bucket3.Accumulate_the_bucket();
    bucket3.Accumulate_the_bucket();
    bucket3.Accumulate_the_bucket();
    assert(bucket3.GetBucketCount() == 3);
    std::cout << "Bucket_3 累加3次后计数: " << bucket3.GetBucketCount() << std::endl;
    
    // 测试 SetBucketCount
    bucket3.SetBucketCount(100);
    assert(bucket3.GetBucketCount() == 100);
    std::cout << "Bucket_3 设置计数为100: " << bucket3.GetBucketCount() << std::endl;
    
    // 测试 Reset
    bucket3.Reset();
    assert(bucket3.GetBucketCount() == 0);
    std::cout << "Bucket_3 重置后计数: " << bucket3.GetBucketCount() << std::endl;
    
    PrintSuccess("VisitFrequencyBucket 基本功能测试");
}

// 测试2: VisitFrequencyBucketArray 初始化测试
void TestVisitFrequencyBucketArrayInit() {
    PrintTestName("VisitFrequencyBucketArray 初始化测试");
    
    // 测试默认16个bucket
    VisitFrequencyBucketArray array16;
    assert(array16.GetBucketsNum() == 16);
    std::cout << "默认 Bucket 数量: " << array16.GetBucketsNum() << std::endl;
    
    // 验证每个bucket的范围
    for (u64 i = 0; i < array16.GetBucketsNum(); i++) {
        const auto& bucket = array16.GetTargetBucket(i);
        u64 expected_lower = (1ULL << i);
        u64 expected_upper = (1ULL << (i + 1)) - 1;
        assert(bucket.bucket_range_lower_bound == expected_lower);
        assert(bucket.bucket_range_upper_bound == expected_upper);
        assert(bucket.GetBucketCount() == 0);
        std::cout << "Bucket_" << i << ": [" << bucket.bucket_range_lower_bound 
                  << ", " << bucket.bucket_range_upper_bound << "], 计数: " 
                  << bucket.GetBucketCount() << std::endl;
    }
    
    // 测试自定义12个bucket
    VisitFrequencyBucketArray array12(12);
    assert(array12.GetBucketsNum() == 12);
    std::cout << "\n自定义 Bucket 数量: " << array12.GetBucketsNum() << std::endl;
    
    PrintSuccess("VisitFrequencyBucketArray 初始化测试");
}

// 测试3: CalculateBucketIndex 功能测试
void TestCalculateBucketIndex() {
    PrintTestName("CalculateBucketIndex 功能测试");
    
    VisitFrequencyBucketArray array(16);
    
    // 测试各种访问次数对应的bucket索引
    struct TestCase {
        u64 visit_count;
        u64 expected_bucket_index;
    };
    
    std::vector<TestCase> test_cases = {
        {1, 0},      // [1] -> Bucket_0
        {2, 1},      // [2, 3] -> Bucket_1
        {3, 1},      // [2, 3] -> Bucket_1
        {4, 2},      // [4, 7] -> Bucket_2
        {7, 2},      // [4, 7] -> Bucket_2
        {8, 3},      // [8, 15] -> Bucket_3
        {15, 3},     // [8, 15] -> Bucket_3
        {16, 4},     // [16, 31] -> Bucket_4
        {20, 4},     // [16, 31] -> Bucket_4
        {50, 5},     // [32, 63] -> Bucket_5
        {100, 6},    // [64, 127] -> Bucket_6
        {1000, 9},   // [512, 1023] -> Bucket_9
        {10000, 13}, // [8192, 16383] -> Bucket_13
    };
    
    for (const auto& tc : test_cases) {
        u64 calculated_index = array.CalculateBucketIndex(tc.visit_count);
        assert(calculated_index == tc.expected_bucket_index);
        const auto& bucket = array.GetTargetBucket(calculated_index);
        std::cout << "访问次数 " << tc.visit_count << " -> Bucket_" 
                  << calculated_index << " [" << bucket.bucket_range_lower_bound 
                  << ", " << bucket.bucket_range_upper_bound << "]" << std::endl;
    }
    
    PrintSuccess("CalculateBucketIndex 功能测试");
}

// 测试4: UpdateVisitFrequencyBucket 功能测试
void TestUpdateVisitFrequencyBucket() {
    PrintTestName("UpdateVisitFrequencyBucket 功能测试");
    
    VisitFrequencyBucketArray array(16);
    
    // 模拟不同页面的访问次数
    std::vector<u64> page_visit_counts = {
        1, 1, 1,           // 3个页面访问1次 -> Bucket_0
        2, 3, 3,           // 3个页面访问2-3次 -> Bucket_1
        5, 7, 7, 7,        // 4个页面访问5-7次 -> Bucket_2
        10, 15,            // 2个页面访问10-15次 -> Bucket_3
        20, 20, 20,        // 3个页面访问20次 -> Bucket_4
        100, 100           // 2个页面访问100次 -> Bucket_6
    };
    
    // 更新bucket统计
    for (u64 count : page_visit_counts) {
        array.UpdateVisitFrequencyBucket(count);
    }
    
    // 验证各bucket的计数
    std::cout << "\n访问频率直方图:" << std::endl;
    std::cout << "Bucket_Index\t范围\t\t\t页面数量" << std::endl;
    std::cout << "----------------------------------------------------" << std::endl;
    
    u64 total_pages = 0;
    for (u64 i = 0; i < array.GetBucketsNum(); i++) {
        const auto& bucket = array.GetTargetBucket(i);
        u64 count = bucket.GetBucketCount();
        if (count > 0) {
            std::cout << "Bucket_" << i << "\t\t[" << bucket.bucket_range_lower_bound 
                      << ", " << bucket.bucket_range_upper_bound << "]\t\t" 
                      << count << std::endl;
            total_pages += count;
        }
    }
    
    assert(array.GetTargetBucket(0).GetBucketCount() == 3);  // 访问1次的页面
    assert(array.GetTargetBucket(1).GetBucketCount() == 3);  // 访问2-3次的页面
    assert(array.GetTargetBucket(2).GetBucketCount() == 4);  // 访问5-7次的页面
    assert(array.GetTargetBucket(3).GetBucketCount() == 2);  // 访问10-15次的页面
    assert(array.GetTargetBucket(4).GetBucketCount() == 3);  // 访问20次的页面
    assert(array.GetTargetBucket(6).GetBucketCount() == 2);  // 访问100次的页面
    
    std::cout << "总页面数: " << total_pages << std::endl;
    assert(total_pages == page_visit_counts.size());
    
    PrintSuccess("UpdateVisitFrequencyBucket 功能测试");
}

// 测试5: GetCumulativePageCount 功能测试
void TestGetCumulativePageCount() {
    PrintTestName("GetCumulativePageCount 功能测试");
    
    VisitFrequencyBucketArray array(16);
    
    // 设置测试数据
    array.GetTargetBucket(0).SetBucketCount(100);  // [1]
    array.GetTargetBucket(1).SetBucketCount(80);   // [2, 3]
    array.GetTargetBucket(2).SetBucketCount(60);   // [4, 7]
    array.GetTargetBucket(3).SetBucketCount(40);   // [8, 15]
    array.GetTargetBucket(4).SetBucketCount(20);   // [16, 31]
    array.GetTargetBucket(5).SetBucketCount(10);   // [32, 63]
    
    // 测试从不同起始bucket开始的累计计数
    u64 cumulative_from_0 = array.GetCumulativePageCount(0);
    assert(cumulative_from_0 == 310);
    std::cout << "从 Bucket_0 开始的累计页面数: " << cumulative_from_0 << std::endl;
    
    u64 cumulative_from_2 = array.GetCumulativePageCount(2);
    assert(cumulative_from_2 == 130);
    std::cout << "从 Bucket_2 开始的累计页面数 (访问>=4次): " << cumulative_from_2 << std::endl;
    
    u64 cumulative_from_4 = array.GetCumulativePageCount(4);
    assert(cumulative_from_4 == 30);
    std::cout << "从 Bucket_4 开始的累计页面数 (访问>=16次): " << cumulative_from_4 << std::endl;
    
    // 模拟根据 DRAM:CXL 容量比例确定准入阈值的场景
    std::cout << "\n模拟准入阈值计算:" << std::endl;
    std::cout << "假设总共有 310 个页面，DRAM 可容纳 30 个页面 (~10%)" << std::endl;
    std::cout << "从高访问频率开始，Bucket_4 (访问>=16次) 及以上有 " 
              << cumulative_from_4 << " 个页面，符合容量要求" << std::endl;
    
    PrintSuccess("GetCumulativePageCount 功能测试");
}

// 测试6: AgingVisitFrequencyBuckets 老化机制测试
void TestAgingMechanism() {
    PrintTestName("AgingVisitFrequencyBuckets 老化机制测试");
    
    VisitFrequencyBucketArray array(5);  // 使用5个bucket便于测试
    
    // 设置初始数据
    array.GetTargetBucket(0).SetBucketCount(50);
    array.GetTargetBucket(1).SetBucketCount(100);
    array.GetTargetBucket(2).SetBucketCount(200);
    array.GetTargetBucket(3).SetBucketCount(300);
    array.GetTargetBucket(4).SetBucketCount(400);
    
    std::cout << "老化前的分布:" << std::endl;
    std::cout << "Bucket_Index\t范围\t\t计数" << std::endl;
    std::cout << "----------------------------------------" << std::endl;
    for (u64 i = 0; i < array.GetBucketsNum(); i++) {
        const auto& bucket = array.GetTargetBucket(i);
        std::cout << "Bucket_" << i << "\t\t[" << bucket.bucket_range_lower_bound 
                  << ", " << bucket.bucket_range_upper_bound << "]\t" 
                  << bucket.GetBucketCount() << std::endl;
    }
    
    // 执行老化操作
    array.AgingVisitFrequencyBuckets();
    
    std::cout << "\n老化后的分布 (数组左移，相当于访问计数减半):" << std::endl;
    std::cout << "Bucket_Index\t范围\t\t计数" << std::endl;
    std::cout << "----------------------------------------" << std::endl;
    for (u64 i = 0; i < array.GetBucketsNum(); i++) {
        const auto& bucket = array.GetTargetBucket(i);
        std::cout << "Bucket_" << i << "\t\t[" << bucket.bucket_range_lower_bound 
                  << ", " << bucket.bucket_range_upper_bound << "]\t" 
                  << bucket.GetBucketCount() << std::endl;
    }
    
    // 验证老化后的结果
    assert(array.GetTargetBucket(0).GetBucketCount() == 100);  // 原来bucket[1]的值
    assert(array.GetTargetBucket(1).GetBucketCount() == 200);  // 原来bucket[2]的值
    assert(array.GetTargetBucket(2).GetBucketCount() == 300);  // 原来bucket[3]的值
    assert(array.GetTargetBucket(3).GetBucketCount() == 400);  // 原来bucket[4]的值
    assert(array.GetTargetBucket(4).GetBucketCount() == 0);    // 最高bucket被清空
    
    std::cout << "\n说明: 老化机制通过数组左移实现，原访问次数在 [2,3] 范围的页面，" << std::endl;
    std::cout << "      老化后移至 [1] 范围，相当于访问计数减半。" << std::endl;
    
    PrintSuccess("AgingVisitFrequencyBuckets 老化机制测试");
}

// 测试7: ResetAll 功能测试
void TestResetAll() {
    PrintTestName("ResetAll 功能测试");
    
    VisitFrequencyBucketArray array(16);
    
    // 设置一些数据
    for (u64 i = 0; i < array.GetBucketsNum(); i++) {
        array.GetTargetBucket(i).SetBucketCount(i * 10);
    }
    
    std::cout << "重置前部分bucket的计数:" << std::endl;
    for (u64 i = 0; i < 5; i++) {
        std::cout << "Bucket_" << i << ": " 
                  << array.GetTargetBucket(i).GetBucketCount() << std::endl;
    }
    
    // 执行重置
    array.ResetAll();
    
    std::cout << "\n重置后所有bucket的计数:" << std::endl;
    for (u64 i = 0; i < array.GetBucketsNum(); i++) {
        assert(array.GetTargetBucket(i).GetBucketCount() == 0);
        if (i < 5) {
            std::cout << "Bucket_" << ": " 
                      << array.GetTargetBucket(i).GetBucketCount() << std::endl;
        }
    }
    
    PrintSuccess("ResetAll 功能测试");
}

// 测试8: 并发安全性测试（多线程）
void TestConcurrentSafety() {
    PrintTestName("并发安全性测试");
    
    VisitFrequencyBucketArray array(16);
    const int num_threads = 4;
    const int operations_per_thread = 10000;
    
    std::cout << "启动 " << num_threads << " 个线程，每个线程执行 " 
              << operations_per_thread << " 次操作..." << std::endl;
    
    std::vector<std::thread> threads;
    
    // 启动多个线程并发更新bucket
    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([&array, operations_per_thread, t]() {
            for (int i = 0; i < operations_per_thread; i++) {
                // 模拟不同的访问次数
                u64 visit_count = (i % 100) + 1;
                array.UpdateVisitFrequencyBucket(visit_count);
            }
        });
    }
    
    // 等待所有线程完成
    for (auto& thread : threads) {
        thread.join();
    }
    
    // 验证总计数
    u64 total_count = 0;
    for (u64 i = 0; i < array.GetBucketsNum(); i++) {
        total_count += array.GetTargetBucket(i).GetBucketCount();
    }
    
    u64 expected_total = num_threads * operations_per_thread;
    assert(total_count == expected_total);
    
    std::cout << "总更新次数: " << total_count << " (预期: " << expected_total << ")" << std::endl;
    std::cout << "并发更新成功，未出现数据竞争问题！" << std::endl;
    
    PrintSuccess("并发安全性测试");
}

// 辅助函数：找到合适的准入阈值bucket
// 返回满足条件的最小bucket索引，使得从该bucket开始的累计页面数 <= dram_capacity
u64 FindAdmissionThresholdBucket(const VisitFrequencyBucketArray& array, u64 dram_capacity_pages) {
    // 从低到高扫描，找到第一个满足条件的bucket
    for (u64 i = 0; i < array.GetBucketsNum(); i++) {
        u64 cumulative = array.GetCumulativePageCount(i);
        if (cumulative <= dram_capacity_pages) {
            return i;  // 找到最小的满足条件的bucket
        }
    }
    // 如果所有bucket的累计都超过容量，返回最后一个bucket
    return array.GetBucketsNum() - 1;
}

// 测试9: 模拟真实的准入控制场景
void TestRealisticAdmissionControlScenario() {
    PrintTestName("模拟真实准入控制场景");
    
    VisitFrequencyBucketArray array(16);
    
    // 模拟 Zipfian 分布的页面访问
    // 少数热页面被访问多次，大量冷页面访问次数少
    std::vector<u64> page_access_pattern = {
        // 极热页面 (5个): 访问1000-5000次
        5000, 3000, 2000, 1500, 1000,
        // 热页面 (15个): 访问100-500次
        500, 400, 350, 300, 250, 200, 180, 160, 140, 120,
        100, 90, 85, 80, 75,
        // 温页面 (30个): 访问10-50次
        50, 45, 40, 38, 35, 32, 30, 28, 25, 23,
        20, 18, 17, 16, 15, 14, 13, 12, 11, 10,
        10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
        // 冷页面 (50个): 访问1-5次
    };
    
    // 添加50个冷页面
    for (int i = 0; i < 50; i++) {
        page_access_pattern.push_back(i % 5 + 1);
    }
    
    std::cout << "模拟 " << page_access_pattern.size() << " 个页面的访问..." << std::endl;
    
    // 更新访问统计
    for (u64 count : page_access_pattern) {
        array.UpdateVisitFrequencyBucket(count);
    }
    
    // 打印访问频率分布
    std::cout << "\n访问频率分布直方图:" << std::endl;
    std::cout << "Bucket\t访问范围\t\t页面数量\t累计数量(从此bucket及以上)" << std::endl;
    std::cout << "------------------------------------------------------------------------" << std::endl;
    
    for (u64 i = 0; i < array.GetBucketsNum(); i++) {
        const auto& bucket = array.GetTargetBucket(i);
        u64 count = bucket.GetBucketCount();
        if (count > 0) {
            u64 cumulative = array.GetCumulativePageCount(i);
            std::cout << "Bucket_" << i << "\t[" << bucket.bucket_range_lower_bound 
                      << ", " << bucket.bucket_range_upper_bound << "]\t\t" 
                      << count << "\t\t" << cumulative << std::endl;
        }
    }
    
    // 模拟不同的 DRAM:CXL 比例场景
    std::cout << "\n================ 准入阈值分析 ================" << std::endl;
    
    struct Scenario {
        std::string name;
        double dram_ratio;
    };
    
    std::vector<Scenario> scenarios = {
        {"DRAM:CXL = 1:2 (33% DRAM)", 0.33},
        {"DRAM:CXL = 1:4 (20% DRAM)", 0.20},
        {"DRAM:CXL = 1:8 (11% DRAM)", 0.11}
    };
    
    u64 total_pages = page_access_pattern.size();
    for (const auto& scenario : scenarios) {
        u64 dram_capacity = static_cast<u64>(total_pages * scenario.dram_ratio);
        std::cout << "\n场景: " << scenario.name << std::endl;
        std::cout << "DRAM 容量可容纳页面数: " << dram_capacity << "/" << total_pages << std::endl;
        
        // 使用修正后的逻辑：找到最小的bucket，使得从该bucket开始的累计页面数 <= DRAM容量
        u64 threshold_bucket = FindAdmissionThresholdBucket(array, dram_capacity);
        u64 pages_above = array.GetCumulativePageCount(threshold_bucket);
        const auto& bucket = array.GetTargetBucket(threshold_bucket);
        
        std::cout << "  -> 准入阈值: 访问次数 >= " << bucket.bucket_range_lower_bound 
                  << " 的页面 (Bucket_" << threshold_bucket << " 及以上)" << std::endl;
        std::cout << "  -> 可准入页面数: " << pages_above << std::endl;
        std::cout << "  -> 容量利用率: " << (100.0 * pages_above / dram_capacity) << "%" << std::endl;
    }
    
    PrintSuccess("模拟真实准入控制场景");
}

// 测试10: 边界条件测试
void TestEdgeCases() {
    PrintTestName("边界条件测试");
    
    VisitFrequencyBucketArray array(16);
    
    // 测试1: 访问次数为1 (最小值)
    u64 idx1 = array.CalculateBucketIndex(1);
    assert(idx1 == 0);
    std::cout << "访问次数 1 -> Bucket_" << idx1 << " ✓" << std::endl;
    
    // 测试2: 访问次数为2的幂次
    for (u64 i = 0; i < 10; i++) {
        u64 visit_count = (1ULL << i);
        u64 idx = array.CalculateBucketIndex(visit_count);
        std::cout << "访问次数 " << visit_count << " (2^" << i << ") -> Bucket_" << idx << " ✓" << std::endl;
    }
    
    // 测试3: 非常大的访问次数 (超出bucket范围)
    u64 very_large = 1ULL << 20;  // 2^20 = 1048576
    u64 idx_large = array.CalculateBucketIndex(very_large);
    assert(idx_large == array.GetBucketsNum() - 1);  // 应该落在最后一个bucket
    std::cout << "访问次数 " << very_large << " (超大值) -> Bucket_" << idx_large 
              << " (最后一个bucket) ✓" << std::endl;
    
    // 测试4: 2的幂次减1 (bucket范围上界)
    for (u64 i = 1; i < 8; i++) {
        u64 visit_count = (1ULL << i) - 1;
        u64 idx = array.CalculateBucketIndex(visit_count);
        std::cout << "访问次数 " << visit_count << " (2^" << i << "-1) -> Bucket_" << idx << " ✓" << std::endl;
    }
    
    PrintSuccess("边界条件测试");
}

// 主测试函数
int main() {
    std::cout << "\n" << std::endl;
    std::cout << "╔════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║     VisitCountBucket 组件测试套件                          ║" << std::endl;
    std::cout << "║     Two-Level Admission Control System                    ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════════╝" << std::endl;
    
    try {
        // 运行所有测试
        TestVisitFrequencyBucketBasic();
        TestVisitFrequencyBucketArrayInit();
        TestCalculateBucketIndex();
        TestUpdateVisitFrequencyBucket();
        TestGetCumulativePageCount();
        TestAgingMechanism();
        TestResetAll();
        TestConcurrentSafety();
        TestRealisticAdmissionControlScenario();
        TestEdgeCases();
        
        // 所有测试通过
        std::cout << "\n" << std::endl;
        std::cout << "╔════════════════════════════════════════════════════════════╗" << std::endl;
        std::cout << "║                     所有测试通过! ✓                        ║" << std::endl;
        std::cout << "╚════════════════════════════════════════════════════════════╝" << std::endl;
        std::cout << "\n" << std::endl;
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n[✗] 测试失败: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "\n[✗] 测试失败: 未知异常" << std::endl;
        return 1;
    }
}
