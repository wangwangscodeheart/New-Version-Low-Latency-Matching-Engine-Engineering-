#include "../src/orderbook.hpp"
#include <iostream>
#include <chrono>
#include <vector>
#include <algorithm>
#include <random>
#include <iomanip>

// ============================================================================
// PERFORMANCE BENCHMARKS
// ============================================================================

class Benchmark {
public:
    static void run_all_benchmarks() {
        std::cout << "\n========== PERFORMANCE BENCHMARKS ==========\n\n";
        
        benchmark_throughput();
        benchmark_latency();
        benchmark_core_latency();
        benchmark_core_batch_latency();
        benchmark_memory();
        benchmark_cancel();
        benchmark_price_level_churn();
    }
    
private:
    static void benchmark_throughput() {
        std::cout << "Benchmark 1: Throughput Test\n";
        const int num_orders = 100000;
        // Pre-allocate capacity to avoid pool exhaustion
        OrderBook book(num_orders * 2); 
        
        auto start = std::chrono::high_resolution_clock::now();
        
        for (int i = 0; i < num_orders; ++i) {
            Side side = (i % 2 == 0) ? Side::BUY : Side::SELL;
            double price = 100.0 + (i % 10) * 0.1;
            book.process_new_order(OrderId(i+1), side, from_double(price), Quantity(10));
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        double throughput = (num_orders * 1000000.0) / duration.count();
        std::cout << "   Processed: " << num_orders << " orders\n";
        std::cout << "   Time: " << duration.count() << " μs\n";
        std::cout << "   Throughput: " << static_cast<size_t>(throughput) << " orders/sec\n";
        std::cout << "   Avg latency: " << (double)duration.count() / num_orders << " μs/order\n\n";
    }
    
    static void benchmark_latency() {
        std::cout << "Benchmark 2: Latency Distribution\n";
        std::cout << "   Workload: 1,000 asks at 100.0-199.9, then 10,000 buys at 105.0 (rest-heavy after sweep)\n";
        // Capacity for setup + test orders
        OrderBook book(20000);
        std::vector<long long> latencies;
        latencies.reserve(10000);
        
        // Pre-populate book
        for (int i = 0; i < 1000; ++i) {
            book.process_new_order(OrderId(i+1), Side::SELL, 
                                 from_double(100.0 + i*0.1), Quantity(10));
        }
        
        // Measure latency of individual orders
        for (int i = 0; i < 10000; ++i) {
            auto start = std::chrono::high_resolution_clock::now();
            book.process_new_order(OrderId(10000+i), Side::BUY, 
                                 from_double(105.0), Quantity(10));
            auto end = std::chrono::high_resolution_clock::now();
            
            latencies.push_back(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()
            );
        }
        
        std::sort(latencies.begin(), latencies.end());
        
        std::cout << "   Samples: " << latencies.size() << "\n";
        std::cout << "   P50: " << latencies[latencies.size()/2] << " ns\n";
        std::cout << "   P90: " << latencies[latencies.size()*9/10] << " ns\n";
        std::cout << "   P99: " << latencies[latencies.size()*99/100] << " ns\n";
        std::cout << "   P99.9: " << latencies[latencies.size()*999/1000] << " ns\n";
        std::cout << "   Max: " << latencies.back() << " ns\n\n";
    }
    
    static void benchmark_memory() {
        std::cout << "Benchmark 5: Memory Usage (Pre-allocated)\n";
        
        const int capacity = 100000;
        OrderBook book(capacity);
        
        // With ObjectPool, we allocate everything upfront.
        size_t pool_size = capacity * sizeof(Order);
        size_t level_pool_size = capacity *
            (sizeof(LimitLevel) + sizeof(size_t) + sizeof(uint8_t));
        // OrderBook reserves up to 2x capacity for NEW/CANCEL plus trade events.
        size_t event_log_size = capacity * 2 * sizeof(Event);
        // Estimate map overhead (rough)
        size_t map_overhead = capacity * 16; 

        std::cout << "   Pool Capacity: " << capacity << "\n";
        std::cout << "   sizeof(Order): " << sizeof(Order) << " bytes (alignment is compiler/platform dependent)\n";
        std::cout << "   Pool Memory: " << pool_size / 1024.0 / 1024.0 << " MB\n";
        std::cout << "   Price-Level Pool Memory: " << level_pool_size / 1024.0 / 1024.0 << " MB\n";
        std::cout << "   Event Log Memory: " << event_log_size / 1024.0 / 1024.0 << " MB\n";
        std::cout << "   Total Pre-allocated: ~" << (pool_size + level_pool_size + event_log_size + map_overhead) / 1024.0 / 1024.0 << " MB\n";
        std::cout << "   Note: Orders, price-level objects, and a 2x event-log baseline are pre-allocated; ordered price-index nodes may still allocate.\n\n";
    }

    static void benchmark_core_latency() {
        std::cout << "Benchmark 3: Core Path Latency (event recording disabled)\n";
        OrderBook book(20000, false);
        std::vector<long long> latencies;
        latencies.reserve(10000);

        for (int i = 0; i < 1000; ++i) {
            book.process_new_order(OrderId(i + 1), Side::SELL,
                                   from_double(100.0 + i * 0.1), Quantity(10));
        }
        for (int i = 0; i < 10000; ++i) {
            auto start = std::chrono::high_resolution_clock::now();
            book.process_new_order(OrderId(10000 + i), Side::BUY,
                                   from_double(105.0), Quantity(10));
            auto end = std::chrono::high_resolution_clock::now();
            latencies.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
        }
        std::sort(latencies.begin(), latencies.end());
        std::cout << "   Same workload as full path, without event-log writes\n";
        std::cout << "   P50: " << latencies[latencies.size() / 2] << " ns\n";
        std::cout << "   P99: " << latencies[latencies.size() * 99 / 100] << " ns\n";
        std::cout << "   P99.9: " << latencies[latencies.size() * 999 / 1000] << " ns\n\n";
    }

    static void benchmark_core_batch_latency() {
        std::cout << "Benchmark 4: Core Batch Average Latency\n";
        constexpr int setup_orders = 1000;
        constexpr int measured_orders = 100000;
        constexpr int repetitions = 7;
        std::vector<double> averages;
        averages.reserve(repetitions);

        for (int repetition = 0; repetition < repetitions; ++repetition) {
            OrderBook book(setup_orders + measured_orders + 100, false);
            for (int i = 0; i < setup_orders; ++i) {
                book.process_new_order(OrderId(i + 1), Side::SELL,
                                       from_double(100.0 + i * 0.1), Quantity(10));
            }

            auto start = std::chrono::high_resolution_clock::now();
            for (int i = 0; i < measured_orders; ++i) {
                book.process_new_order(OrderId(10000 + i), Side::BUY,
                                       from_double(105.0), Quantity(10));
            }
            auto end = std::chrono::high_resolution_clock::now();
            auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            averages.push_back(static_cast<double>(elapsed_ns) / measured_orders);
        }

        std::sort(averages.begin(), averages.end());
        std::cout << "   Orders per repetition: " << measured_orders << "\n";
        std::cout << "   Repetitions: " << repetitions << "\n";
        std::cout << "   Min: " << averages.front() << " ns/order\n";
        std::cout << "   Median: " << averages[averages.size() / 2] << " ns/order\n";
        std::cout << "   Max: " << averages.back() << " ns/order\n\n";
    }
    
    static void benchmark_cancel() {
        std::cout << "Benchmark 6: Cancel Performance\n";
        const int num_orders = 10000;
        OrderBook book(num_orders * 2);
        std::vector<OrderId> order_ids;
        order_ids.reserve(num_orders);
        
        // Add many orders
        for (int i = 0; i < num_orders; ++i) {
            Side side = (i % 2 == 0) ? Side::BUY : Side::SELL;
            double price = 100.0 + (i % 100) * 0.1;
            book.process_new_order(OrderId(i+1), side, from_double(price), Quantity(10));
            order_ids.push_back(OrderId(i+1));
        }
        
        // Benchmark cancellations
        auto start = std::chrono::high_resolution_clock::now();
        
        for (int i = 0; i < 1000; ++i) {
            book.process_cancel(order_ids[i]);
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        std::cout << "   Cancelled: 1000 orders\n";
        std::cout << "   Time: " << duration.count() << " μs\n";
        std::cout << "   Avg per cancel: " << (double)duration.count() / 1000.0 << " μs\n";
        std::cout << "   Note: fixed-index lookup + intrusive O(1) unlink; level metadata cleanup may touch the price map.\n\n";
    }

    static void benchmark_price_level_churn() {
        std::cout << "Benchmark 7: Price-Level Churn\n";
        const int rounds = 20000;
        OrderBook book(rounds + 100, false);
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < rounds; ++i) {
            const OrderId id(static_cast<uint64_t>(i + 1));
            const double price = 50.0 + static_cast<double>(i % 10000) * 0.01;
            book.process_new_order(id, Side::BUY, from_double(price), Quantity(1));
            book.process_cancel(id);
        }
        auto end = std::chrono::high_resolution_clock::now();
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        std::cout << "   Created/destroyed: " << rounds << " price levels\n";
        std::cout << "   Avg round-trip: " << (static_cast<double>(ns) / rounds) << " ns\n\n";
        std::cout << "   Residual levels: bid=" << book.bid_level_count()
                  << ", ask=" << book.ask_level_count() << "\n\n";
    }
};

// ============================================================================
// STRESS TEST
// ============================================================================

class StressTest {
public:
    static void run_stress_test() {
        std::cout << "\n========== STRESS TEST ==========\n\n";
        
        const int TOTAL_OPS = 1000000;
        std::cout << "Running 1 million order test...\n";
        
        // IMPORTANT: Pre-allocate enough space for the stress test
        // ObjectPool does NOT resize to guarantee pointer validity.
        OrderBook book(TOTAL_OPS + 100000);
        
        auto start = std::chrono::high_resolution_clock::now();
        
        for (int i = 0; i < TOTAL_OPS; ++i) {
            Side side = (i % 3 == 0) ? Side::BUY : Side::SELL;
            double price = 100.0 + (i % 50) * 0.01;
            
            // i+1 to avoid OrderId(0) if that's reserved
            book.process_new_order(OrderId(i+1), side, from_double(price), Quantity(i % 100 + 1));
            
            // Periodic cancellations
            if (i % 100 == 0 && i > 50) {
                book.process_cancel(OrderId(i - 50));
            }
            
            // Periodic invariant checks (expensive, so only every 10k)
            // if (i % 10000 == 0) {
            //     if (!book.check_invariants()) throw std::runtime_error("Invariant failed");
            // }
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        if (!book.check_invariants()) {
            throw std::runtime_error("Stress-test final order-book invariant failure");
        }
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        
        std::cout << "   ✓ Processed 1,000,000 orders\n";
        std::cout << "   Time: " << duration.count() / 1000.0 << " seconds\n";
        std::cout << "   Throughput: " << (size_t)(TOTAL_OPS * 1000.0 / duration.count()) << " orders/sec\n";
    }

private:
    static Price from_double(double p) {
        return ::from_double(p);
    }
};

// ============================================================================
// COMPARISON TEST
// ============================================================================

class ComparisonTest {
public:
    static void compare_scenarios() {
        std::cout << "\n========== SCENARIO COMPARISON ==========\n\n";
        
        std::cout << "Scenario 1: All orders match immediately\n";
        benchmark_scenario_all_match();
        
        std::cout << "\nScenario 2: All orders rest on book\n";
        benchmark_scenario_all_rest();
        
        std::cout << "\nScenario 3: Mixed (50% match, 50% rest)\n";
        benchmark_scenario_mixed();
    }
    
private:
    static Price from_double(double p) {
        return ::from_double(p);
    }

    static void benchmark_scenario_all_match() {
        const int num_pairs = 50000;
        OrderBook book(num_pairs * 2 + 1000);
        
        auto start = std::chrono::high_resolution_clock::now();
        
        for (int i = 0; i < num_pairs; ++i) {
            book.process_new_order(OrderId(i*2+1), Side::SELL, from_double(100.0), Quantity(10));
            book.process_new_order(OrderId(i*2+2), Side::BUY, from_double(100.0), Quantity(10));
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        
        std::cout << "   Time: " << duration.count() << " ms\n";
        std::cout << "   Throughput: " << (num_pairs * 2 * 1000) / (duration.count() + 1) // +1 avoid div by zero
                  << " orders/sec\n";
    }
    
    static void benchmark_scenario_all_rest() {
        const int num_orders = 100000;
        OrderBook book(num_orders + 1000);
        
        auto start = std::chrono::high_resolution_clock::now();
        
        for (int i = 0; i < num_orders; ++i) {
            Side side = (i % 2 == 0) ? Side::BUY : Side::SELL;
            double price = side == Side::BUY ? 99.0 : 101.0;
            book.process_new_order(OrderId(i+1), side, from_double(price), Quantity(10));
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        
        std::cout << "   Time: " << duration.count() << " ms\n";
        std::cout << "   Throughput: " << (num_orders * 1000) / (duration.count() + 1)
                  << " orders/sec\n";
    }
    
    static void benchmark_scenario_mixed() {
        const int num_orders = 100000;
        OrderBook book(num_orders + 1000);
        
        auto start = std::chrono::high_resolution_clock::now();
        
        for (int i = 0; i < num_orders; ++i) {
            Side side = (i % 2 == 0) ? Side::BUY : Side::SELL;
            // Orders 0, 1 match. 2, 3 rest.
            double price = (i % 4 < 2) ? 100.0 : (side == Side::BUY ? 99.0 : 101.0);
            book.process_new_order(OrderId(i+1), side, from_double(price), Quantity(10));
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        
        std::cout << "   Time: " << duration.count() << " ms\n";
        std::cout << "   Throughput: " << (num_orders * 1000) / (duration.count() + 1)
                  << " orders/sec\n";
    }
};

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "╔════════════════════════════════════════════════════════════╗\n";
    std::cout << "║           PERFORMANCE BENCHMARK SUITE                      ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════╝\n";
    
    try {
        Benchmark::run_all_benchmarks();
        ComparisonTest::compare_scenarios();
        StressTest::run_stress_test();
        
        std::cout << "\n✅ All benchmarks completed successfully!\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Benchmark failed with exception: " << e.what() << "\n";
        return 1;
    }
}
