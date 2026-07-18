#include "../src/orderbook.hpp"
#include "../src/replay.hpp"
#include <iostream>
#include <random>
#include <cassert>
#include <variant>
#include <vector>
#include <algorithm> // For std::min

// ============================================================================
// CUSTOM ASSERTION MACRO
// ============================================================================
#define TEST_ASSERT(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "❌ Assertion failed: " << #cond \
                      << "\n   File: " << __FILE__ \
                      << "\n   Line: " << __LINE__ << std::endl; \
            throw std::runtime_error("Property check failed"); \
        } \
    } while(0)

// ============================================================================
// PROPERTY-BASED TESTING (QuickCheck style)
// ============================================================================

class PropertyTesting {
private:
    // Fixed seed makes failures reproducible from a test report.
    std::mt19937 rng{0xC0FFEEu};
    
    struct RandomOrder {
        OrderId id;
        Side side;
        Price price;
        Quantity quantity;
    };
    
    RandomOrder generate_random_order(uint64_t id) {
        std::uniform_int_distribution<> side_dist(0, 1);
        std::uniform_real_distribution<> price_dist(95.0, 105.0);
        std::uniform_int_distribution<uint64_t> qty_dist(1, 1000);
        
        // Convert floating point random price to fixed-point int64
        // Round to 2 decimal places then scale
        double p = std::round(price_dist(rng) * 100.0) / 100.0;
        
        return {
            OrderId(id),
            side_dist(rng) == 0 ? Side::BUY : Side::SELL,
            from_double(p),
            Quantity(qty_dist(rng))
        };
    }
    
public:
    // Property 1: The Order Book Non-Crossing Principle
    void test_never_crosses() {
        std::cout << "\n🔬 Property Test 1: Book Never Crosses\n";
        
        for (int trial = 0; trial < 100; ++trial) { // Reduced trials for speed
            OrderBook book(2000); // Pre-allocate
            
            for (uint64_t i = 0; i < 100; ++i) {
                auto order = generate_random_order(trial * 100 + i + 1);
                book.process_new_order(order.id, order.side, order.price, order.quantity);
                TEST_ASSERT(book.check_invariants());
                
                // Invariant Check: Best Bid < Best Ask
                auto bid = book.best_bid();
                auto ask = book.best_ask();
                
                if (bid.has_value() && ask.has_value()) {
                    if (bid->get() >= ask->get()) {
                        std::cerr << "FAILED at trial " << trial << ", order " << i << "\n";
                        std::cerr << "Bid: " << bid->get() << " >= Ask: " << ask->get() << "\n";
                        throw std::runtime_error("Invariant violation detected: Crossed Book");
                    }
                }
            }
        }
        
        std::cout << "   ✓ Passed randomized crossing checks\n";
    }
    
    // Property 2: Replay Determinism
    void test_replay_idempotence() {
        std::cout << "\n🔬 Property Test 2: Replay Idempotence\n";
        
        for (int trial = 0; trial < 50; ++trial) {
            OrderBook book1(1000); // Pre-allocate
            std::vector<Command> commands;
            commands.reserve(50);
            
            for (uint64_t i = 0; i < 50; ++i) {
                auto order = generate_random_order(trial * 50 + i + 1);
                commands.emplace_back(std::in_place_type<NewOrderCommand>,
                                      CommandSequence(i + 1), order.id, order.side,
                                      order.price, order.quantity);
                book1.process(commands.back());
            }
            
            OrderBook book2 = ReplayEngine::replay_commands(commands);
            TEST_ASSERT(book1.check_invariants());
            TEST_ASSERT(book2.check_invariants());
            TEST_ASSERT(book1.state_hash() == book2.state_hash());
            
            auto bid1 = book1.best_bid();
            auto bid2 = book2.best_bid();
            auto ask1 = book1.best_ask();
            auto ask2 = book2.best_ask();
            
            TEST_ASSERT(bid1.has_value() == bid2.has_value());
            TEST_ASSERT(ask1.has_value() == ask2.has_value());
            
            // Exact integer comparison
            if (bid1.has_value()) {
                TEST_ASSERT(bid1->get() == bid2->get());
            }
            if (ask1.has_value()) {
                TEST_ASSERT(ask1->get() == ask2->get());
            }
        }
        
        std::cout << "   ✓ Passed 50 replay trials\n";
    }
    
    // Property 3: Conservation of Execution Volume
    void test_volume_conservation() {
        std::cout << "\n🔬 Property Test 3: Volume Conservation\n";
        
        for (int trial = 0; trial < 100; ++trial) {
            OrderBook book(2000);
            
            uint64_t total_buy = 0;
            uint64_t total_sell = 0;
            
            for (uint64_t i = 0; i < 100; ++i) {
                auto order = generate_random_order(trial * 100 + i + 1);
                book.process_new_order(order.id, order.side, order.price, order.quantity);
                
                if (order.side == Side::BUY) {
                    total_buy += order.quantity.get();
                } else {
                    total_sell += order.quantity.get();
                }
            }
            
            uint64_t traded = 0;
            for (const auto& event : book.get_event_log()) {
                // Use std::get_if for std::variant
                if (auto trade = std::get_if<TradeEvent>(&event)) {
                    traded += trade->quantity.get();
                }
            }
            
            // Total traded quantity cannot exceed the total quantity submitted on either side
            TEST_ASSERT(traded <= total_buy);
            TEST_ASSERT(traded <= total_sell);
        }
        
        std::cout << "   ✓ Volume conservation verified\n";
    }
    
    // Property 4: Priority Ordering (FIFO)
    void test_fifo_order() {
        std::cout << "\n🔬 Property Test 4: FIFO Priority\n";
        
        OrderBook book(1000);
        Price price(100 * PRICE_SCALE);
        
        // Submit 10 Sell orders at same price
        for (uint64_t i = 0; i < 10; ++i) {
            book.process_new_order(OrderId(i+1), Side::SELL, price, Quantity(10));
        }
        
        // Submit 1 huge Buy order to sweep them all
        book.process_new_order(OrderId(100), Side::BUY, price, Quantity(100));
        
        std::vector<uint64_t> trade_sequence;
        for (const auto& event : book.get_event_log()) {
            if (auto trade = std::get_if<TradeEvent>(&event)) {
                trade_sequence.push_back(trade->passive_order_id.get());
            }
        }
        
        // Verify they matched in order 1, 2, 3... 10
        TEST_ASSERT(trade_sequence.size() == 10);
        for (size_t i = 0; i < trade_sequence.size(); ++i) {
            // Expected ID is i+1
            if (trade_sequence[i] != i + 1) {
                std::cerr << "FIFO Violation: Expected Order " << (i+1) 
                          << ", got " << trade_sequence[i] << "\n";
                throw std::runtime_error("FIFO violation");
            }
        }
        
        std::cout << "   ✓ FIFO order maintained\n";
    }
    
    // Property 5: Best Price Monotonicity
    void test_price_monotonicity() {
        std::cout << "\n🔬 Property Test 5: Price Reasonableness\n";
        
        OrderBook book(2000);
        
        // Seed book
        book.process_new_order(OrderId(1), Side::BUY, Price(99 * PRICE_SCALE), Quantity(100));
        book.process_new_order(OrderId(2), Side::SELL, Price(101 * PRICE_SCALE), Quantity(100));
        
        for (int i = 0; i < 100; ++i) {
            auto order = generate_random_order(10 + i);
            book.process_new_order(order.id, order.side, order.price, order.quantity);
            
            auto bid = book.best_bid();
            auto ask = book.best_ask();
            
            if (bid.has_value() && ask.has_value()) {
                // Spread must be non-negative (Ask >= Bid)
                if (ask->get() < bid->get()) {
                    throw std::runtime_error("Negative spread detected!");
                }
            }
        }
        
        std::cout << "   ✓ Price spread remains non-negative\n";
    }
    
    void run_all() {
        std::cout << "\n" << std::string(60, '=') << "\n";
        std::cout << "PROPERTY-BASED TEST SUITE\n";
        std::cout << std::string(60, '=') << "\n";
        
        test_never_crosses();
        test_replay_idempotence();
        test_volume_conservation();
        test_fifo_order();
        test_price_monotonicity();
        
        std::cout << "\n✅ All property tests passed!\n";
    }
};

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "╔════════════════════════════════════════════════════════════╗\n";
    std::cout << "║           PROPERTY-BASED TEST SUITE                        ║\n";
    std::cout << "║   (QuickCheck-style randomized testing)                    ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════╝\n";
    
    try {
        PropertyTesting tests;
        tests.run_all();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << "\n";
        return 1;
    }
}
