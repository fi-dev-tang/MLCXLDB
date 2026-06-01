#include "backend/leanstore/storage/two-level-admission-control/CountMinSketch.hpp"
#include <iostream>
#include <thread>
#include <vector>
#include <cassert>

using namespace leanstore::storage::two_level_admission_control;

void testPageCMS() {
    std::cout << "=== Testing Page Count-Min-Sketch ===" << std::endl;
    
    PageCountMinSketch page_cms(12, 1024 * 16);
    
    std::cout << "Configuration: " 
              << page_cms.CMSGetRowNum() << " rows x " 
              << page_cms.CMSGetColNum() << " cols" << std::endl;
    std::cout << "Memory Usage: " 
              << page_cms.CMSGetMemoryUsage() / 1024.0 / 1024.0 
              << " MB" << std::endl;
    
    // 单线程测试
    std::cout << "\n--- Single Thread Test ---" << std::endl;
    page_cms.CMSPageAccessUpdate(12345);
    page_cms.CMSPageAccessUpdate(12345);
    page_cms.CMSPageAccessUpdate(67890);
    
    u64 count_12345 = page_cms.CMSGetPageAccessCount(12345);
    u64 count_67890 = page_cms.CMSGetPageAccessCount(67890);
    
    std::cout << "Page 12345 访问次数: " << count_12345 << " (期望: 2)" << std::endl;
    std::cout << "Page 67890 访问次数: " << count_67890 << " (期望: 1)" << std::endl;
    
    assert(count_12345 == 2);
    assert(count_67890 == 1);
    
    // Aging 测试
    std::cout << "\n--- Aging Test ---" << std::endl;
    page_cms.CMSAging();
    u64 count_after_aging = page_cms.CMSGetPageAccessCount(12345);
    std::cout << "After Aging, Page 12345 访问次数: " << count_after_aging 
              << " (期望: 1)" << std::endl;
    assert(count_after_aging == 1);
    
    // 多线程测试
    std::cout << "\n--- Multi-Thread Test ---" << std::endl;
    page_cms.CMSReset();
    
    const int num_threads = 4;
    const int iterations = 1000;
    std::vector<std::thread> threads;
    
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back([&page_cms, iterations]() {
            for (int j = 0; j < iterations; j++) {
                page_cms.CMSPageAccessUpdate(12345);
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    u64 count_mt = page_cms.CMSGetPageAccessCount(12345);
    std::cout << num_threads << "个线程各访问" << iterations << "次后，Page 12345 访问次数: " 
              << count_mt << " (期望: " << num_threads * iterations << ")" << std::endl;
    assert(count_mt == num_threads * iterations);
    
    std::cout << "✓ Page CMS 测试通过!\n" << std::endl;
}

void testRecordCMS() {
    std::cout << "=== Testing Record Count-Min-Sketch ===" << std::endl;
    
    RecordCountMinSketch record_cms(8, 1024 * 8);
    
    std::cout << "Configuration: " 
              << record_cms.CMSGetRowNum() << " rows x " 
              << record_cms.CMSGetColNum() << " cols" << std::endl;
    std::cout << "Memory Usage: " 
              << record_cms.CMSGetMemoryUsage() / 1024.0 / 1024.0 
              << " MB" << std::endl;
    
    // 记录访问
    record_cms.CMSRecordAccessUpdate(12345, 10);
    record_cms.CMSRecordAccessUpdate(12345, 10);
    record_cms.CMSRecordAccessUpdate(12345, 10);
    record_cms.CMSRecordAccessUpdate(12345, 20);
    
    u64 count_slot_10 = record_cms.CMSGetRecordAccessCount(12345, 10);
    u64 count_slot_20 = record_cms.CMSGetRecordAccessCount(12345, 20);
    
    std::cout << "Record (12345, 10) 访问次数: " << count_slot_10 << " (期望: 3)" << std::endl;
    std::cout << "Record (12345, 20) 访问次数: " << count_slot_20 << " (期望: 1)" << std::endl;
    
    assert(count_slot_10 == 3);
    assert(count_slot_20 == 1);
    
    // Aging 测试
    record_cms.CMSAging();
    u64 count_after_aging = record_cms.CMSGetRecordAccessCount(12345, 10);
    std::cout << "After Aging, Record (12345, 10) 访问次数: " << count_after_aging 
              << " (期望: 1)" << std::endl;
    assert(count_after_aging == 1);
    
    std::cout << "✓ Record CMS 测试通过!\n" << std::endl;
}

int main() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "  Count-Min-Sketch 测试程序" << std::endl;
    std::cout << "========================================\n" << std::endl;
    
    try {
        testPageCMS();
        testRecordCMS();
        
        std::cout << "========================================" << std::endl;
        std::cout << "  ✓ 所有测试通过!" << std::endl;
        std::cout << "========================================" << std::endl;
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "✗ 测试失败: " << e.what() << std::endl;
        return 1;
    }
}
