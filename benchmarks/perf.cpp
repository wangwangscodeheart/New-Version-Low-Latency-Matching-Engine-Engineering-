#include "../src/orderbook.hpp"
#include <iostream>
#include <chrono>
#include <vector>
#include <algorithm>
#include <random>
#include <iomanip>
#include <stdexcept>

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
        benchmark_hot_path_matrix();
        benchmark_memory();
        benchmark_cancel();
        benchmark_price_level_churn();
    }
    
private:
    static inline uint64_t benchmark_checksum_ = 0;

    static void consume_state(const OrderBook& book) {
        const uint64_t value = book.state_hash();
        benchmark_checksum_ ^= value + 0x9e3779b97f4a7c15ULL +
                               (benchmark_checksum_ << 6U) +
                               (benchmark_checksum_ >> 2U);
    }

    struct BatchSummary {
        double minimum;
        double median;
        double maximum;
    };

    template <typename RunOnce>
    static BatchSummary measure_repetitions(int repetitions, RunOnce&& run_once) {
        std::vector<double> samples;
        samples.reserve(static_cast<size_t>(repetitions));
        for (int repetition = 0; repetition < repetitions; ++repetition) {
            samples.push_back(run_once());
        }
        std::sort(samples.begin(), samples.end());
        return BatchSummary{
            samples.front(), samples[samples.size() / 2], samples.back()
        };
    }

    static void print_matrix_result(const char* scenario, size_t operations,
                                    int repetitions, const BatchSummary& result) {
        std::cout << "   " << scenario << ": min=" << result.minimum
                  << " ns, median=" << result.median
                  << " ns, max=" << result.maximum << " ns/order\n";
        // Stable, machine-readable output for before/after data capture.
        std::cout << "RESULT_CSV," << scenario << ',' << operations << ','
                  << repetitions << ',' << result.minimum << ','
                  << result.median << ',' << result.maximum << "\n";
    }

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
        std::cout << "Benchmark 6: Memory Usage (Pre-allocated)\n";
        
        const int capacity = 100000;
        OrderBook book(capacity);
        
        // With ObjectPool, we allocate everything upfront.
        size_t pool_size = capacity * sizeof(Order);
        size_t level_pool_size = capacity *
            (sizeof(LimitLevel) + sizeof(size_t) + sizeof(uint8_t));
        // OrderBook reserves up to 2x capacity for NEW/CANCEL plus trade events.
        size_t event_log_size = capacity * 2 * sizeof(EngineEvent);
        // Estimate map overhead (rough)
        size_t map_overhead = capacity * 16; 

        std::cout << "   Pool Capacity: " << capacity << "\n";
        std::cout << "   sizeof(Order): " << sizeof(Order) << " bytes (alignment is compiler/platform dependent)\n";
        std::cout << "   Fixed Index Slot: " << FixedOrderIndex::slot_size_bytes() << " bytes\n";
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

    static void benchmark_hot_path_matrix() {
        std::cout << "Benchmark 5: Hot-Path Scenario Matrix (events disabled)\n";
        std::cout << "   Timings exclude setup and invariant checks.\n";
        constexpr int repetitions = 7;

        constexpr int resting_operations = 100000;
        const BatchSummary same_price = measure_repetitions(repetitions, [=] {
            OrderBook book(resting_operations + 100, false);
            const Price price = from_double(100.00);
            book.process_new_order(OrderId(1), Side::BUY, price, Quantity(1));
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < resting_operations; ++i) {
                book.process_new_order(OrderId(static_cast<uint64_t>(i) + 2),
                                       Side::BUY, price, Quantity(1));
            }
            const auto end = std::chrono::steady_clock::now();
            if (!book.check_invariants()) {
                throw std::runtime_error("same-price benchmark invariant failure");
            }
            consume_state(book);
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            return static_cast<double>(elapsed) / resting_operations;
        });
        print_matrix_result("same_price_existing_level", resting_operations,
                            repetitions, same_price);

        constexpr int active_prices = 64;
        const BatchSummary multiple_prices = measure_repetitions(repetitions, [=] {
            OrderBook book(resting_operations + active_prices + 100, false);
            std::vector<Price> prices;
            prices.reserve(active_prices);
            for (int i = 0; i < active_prices; ++i) {
                prices.emplace_back(1'000'000 - i * 100);
                book.process_new_order(OrderId(static_cast<uint64_t>(i) + 1),
                                       Side::BUY, prices[static_cast<size_t>(i)], Quantity(1));
            }
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < resting_operations; ++i) {
                book.process_new_order(OrderId(static_cast<uint64_t>(i) + 1'000),
                                       Side::BUY,
                                       prices[static_cast<size_t>(i % active_prices)],
                                       Quantity(1));
            }
            const auto end = std::chrono::steady_clock::now();
            if (!book.check_invariants()) {
                throw std::runtime_error("multiple-price benchmark invariant failure");
            }
            consume_state(book);
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            return static_cast<double>(elapsed) / resting_operations;
        });
        print_matrix_result("multiple_existing_levels", resting_operations,
                            repetitions, multiple_prices);

        constexpr int new_level_operations = 10000;
        const BatchSummary new_levels = measure_repetitions(repetitions, [=] {
            OrderBook book(new_level_operations + 100, false);
            std::vector<Price> prices;
            prices.reserve(new_level_operations);
            for (int i = 0; i < new_level_operations; ++i) {
                prices.emplace_back(1'000'000 + static_cast<int64_t>(i) * 100);
            }
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < new_level_operations; ++i) {
                book.process_new_order(OrderId(static_cast<uint64_t>(i) + 1),
                                       Side::BUY, prices[static_cast<size_t>(i)], Quantity(1));
            }
            const auto end = std::chrono::steady_clock::now();
            if (!book.check_invariants()) {
                throw std::runtime_error("new-level benchmark invariant failure");
            }
            consume_state(book);
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            return static_cast<double>(elapsed) / new_level_operations;
        });
        print_matrix_result("new_price_level", new_level_operations,
                            repetitions, new_levels);

        constexpr int matching_operations = 100000;
        const BatchSummary immediate_match = measure_repetitions(repetitions, [=] {
            OrderBook book(matching_operations + 100, false);
            const Price price = from_double(100.00);
            for (int i = 0; i < matching_operations; ++i) {
                book.process_new_order(OrderId(static_cast<uint64_t>(i) + 1),
                                       Side::SELL, price, Quantity(1));
            }
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < matching_operations; ++i) {
                book.process_new_order(
                    OrderId(static_cast<uint64_t>(matching_operations + i) + 1),
                    Side::BUY, price, Quantity(1));
            }
            const auto end = std::chrono::steady_clock::now();
            if (!book.check_invariants()) {
                throw std::runtime_error("immediate-match benchmark invariant failure");
            }
            consume_state(book);
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            return static_cast<double>(elapsed) / matching_operations;
        });
        print_matrix_result("immediate_match", matching_operations,
                            repetitions, immediate_match);

        constexpr int sweep_operations = 10000;
        constexpr int levels_per_sweep = 4;
        const BatchSummary multi_level_sweep = measure_repetitions(repetitions, [=] {
            const int passive_orders = sweep_operations * levels_per_sweep;
            OrderBook book(passive_orders + sweep_operations + 100, false);
            for (int i = 0; i < passive_orders; ++i) {
                book.process_new_order(OrderId(static_cast<uint64_t>(i) + 1),
                                       Side::SELL,
                                       Price(1'000'000 + static_cast<int64_t>(i) * 100),
                                       Quantity(1));
            }
            const Price sweep_price(1'000'000 +
                                    static_cast<int64_t>(passive_orders - 1) * 100);
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < sweep_operations; ++i) {
                book.process_new_order(
                    OrderId(static_cast<uint64_t>(passive_orders + i) + 1),
                    Side::BUY, sweep_price, Quantity(levels_per_sweep));
            }
            const auto end = std::chrono::steady_clock::now();
            if (!book.check_invariants()) {
                throw std::runtime_error("multi-level sweep benchmark invariant failure");
            }
            consume_state(book);
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            return static_cast<double>(elapsed) / sweep_operations;
        });
        print_matrix_result("four_level_sweep", sweep_operations,
                            repetitions, multi_level_sweep);

        const BatchSummary partial_fill = measure_repetitions(repetitions, [=] {
            OrderBook book(matching_operations + 100, false);
            const Price price = from_double(100.00);
            for (int i = 0; i < matching_operations; ++i) {
                book.process_new_order(OrderId(static_cast<uint64_t>(i) + 1),
                                       Side::SELL, price, Quantity(2));
            }
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < matching_operations; ++i) {
                book.process_new_order(
                    OrderId(static_cast<uint64_t>(matching_operations + i) + 1),
                    Side::BUY, price, Quantity(1));
            }
            const auto end = std::chrono::steady_clock::now();
            if (!book.check_invariants() ||
                book.active_order_count() != static_cast<size_t>(matching_operations)) {
                throw std::runtime_error("partial-fill benchmark state failure");
            }
            consume_state(book);
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            return static_cast<double>(elapsed) / matching_operations;
        });
        print_matrix_result("partial_fill", matching_operations,
                            repetitions, partial_fill);

        constexpr int cancel_operations = 30000;
        const BatchSummary cancel_middle = measure_repetitions(repetitions, [=] {
            OrderBook book(cancel_operations * 3 + 100, false);
            const Price price = from_double(99.00);
            for (int i = 0; i < cancel_operations * 3; ++i) {
                book.process_new_order(OrderId(static_cast<uint64_t>(i) + 1),
                                       Side::BUY, price, Quantity(1));
            }
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < cancel_operations; ++i) {
                book.process_cancel(OrderId(static_cast<uint64_t>(i * 3) + 2));
            }
            const auto end = std::chrono::steady_clock::now();
            if (!book.check_invariants() ||
                book.active_order_count() != static_cast<size_t>(cancel_operations * 2)) {
                throw std::runtime_error("cancel-middle benchmark state failure");
            }
            consume_state(book);
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            return static_cast<double>(elapsed) / cancel_operations;
        });
        print_matrix_result("cancel_middle", cancel_operations,
                            repetitions, cancel_middle);

        constexpr int cancel_level_operations = 10000;
        const BatchSummary cancel_last_level = measure_repetitions(repetitions, [=] {
            OrderBook book(cancel_level_operations + 100, false);
            for (int i = 0; i < cancel_level_operations; ++i) {
                book.process_new_order(OrderId(static_cast<uint64_t>(i) + 1), Side::BUY,
                                       Price(1'000'000 + static_cast<int64_t>(i) * 100),
                                       Quantity(1));
            }
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < cancel_level_operations; ++i) {
                book.process_cancel(OrderId(static_cast<uint64_t>(i) + 1));
            }
            const auto end = std::chrono::steady_clock::now();
            if (!book.check_invariants() || book.active_order_count() != 0 ||
                book.bid_level_count() != 0) {
                throw std::runtime_error("cancel-last-level benchmark state failure");
            }
            consume_state(book);
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            return static_cast<double>(elapsed) / cancel_level_operations;
        });
        print_matrix_result("cancel_last_level", cancel_level_operations,
                            repetitions, cancel_last_level);

        constexpr int rejection_operations = 100000;
        const BatchSummary duplicate_reject = measure_repetitions(repetitions, [=] {
            OrderBook book(100, false);
            const Price price = from_double(100.00);
            book.process_new_order(OrderId(1), Side::BUY, price, Quantity(1));
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < rejection_operations; ++i) {
                book.process_new_order(OrderId(1), Side::BUY, price, Quantity(1));
            }
            const auto end = std::chrono::steady_clock::now();
            if (!book.check_invariants() || book.active_order_count() != 1) {
                throw std::runtime_error("duplicate-reject benchmark state failure");
            }
            consume_state(book);
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            return static_cast<double>(elapsed) / rejection_operations;
        });
        print_matrix_result("duplicate_reject", rejection_operations,
                            repetitions, duplicate_reject);

        const BatchSummary invalid_reject = measure_repetitions(repetitions, [=] {
            OrderBook book(100, false);
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < rejection_operations; ++i) {
                book.process_new_order(OrderId(static_cast<uint64_t>(i) + 1),
                                       Side::BUY, Price(0), Quantity(1));
            }
            const auto end = std::chrono::steady_clock::now();
            if (!book.check_invariants() || book.active_order_count() != 0) {
                throw std::runtime_error("invalid-reject benchmark state failure");
            }
            consume_state(book);
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            return static_cast<double>(elapsed) / rejection_operations;
        });
        print_matrix_result("invalid_reject", rejection_operations,
                            repetitions, invalid_reject);

        const BatchSummary pool_exhausted = measure_repetitions(repetitions, [=] {
            OrderBook book(1, false);
            const Price price = from_double(100.00);
            book.process_new_order(OrderId(1), Side::BUY, price, Quantity(1));
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < rejection_operations; ++i) {
                book.process_new_order(OrderId(static_cast<uint64_t>(i) + 2),
                                       Side::BUY, price, Quantity(1));
            }
            const auto end = std::chrono::steady_clock::now();
            if (!book.check_invariants() || book.active_order_count() != 1) {
                throw std::runtime_error("pool-exhausted benchmark state failure");
            }
            consume_state(book);
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            return static_cast<double>(elapsed) / rejection_operations;
        });
        print_matrix_result("pool_exhausted", rejection_operations,
                            repetitions, pool_exhausted);
        std::cout << "MATRIX_CHECKSUM," << benchmark_checksum_ << '\n';
        std::cout << '\n';
    }
    
    static void benchmark_cancel() {
        std::cout << "Benchmark 7: Cancel Performance\n";
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
        std::cout << "Benchmark 8: Price-Level Churn\n";
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
