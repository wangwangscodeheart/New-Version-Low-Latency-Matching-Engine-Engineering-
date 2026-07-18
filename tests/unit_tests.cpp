#include "../src/orderbook.hpp"
#include "../src/replay.hpp"
#include "../src/limit_level_pool.hpp"
#include <iostream>
#include <cassert>
#include <variant>
#include <vector>
#include <filesystem>
#include <fstream>

// ============================================================================
// CUSTOM ASSERTION MACRO (Works in Release Mode)
// ============================================================================
#define TEST_ASSERT(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "❌ Assertion failed: " << #cond \
                      << "\n   File: " << __FILE__ \
                      << "\n   Line: " << __LINE__ << std::endl; \
            throw std::runtime_error("Test assertion failed"); \
        } \
    } while(0)

// ============================================================================
// UNIT TEST SUITE
// ============================================================================

class TestSuite {
public:
    static void run_all_tests() {
        std::cout << "Running Matching Engine Test Suite...\n\n";
        
        try {
            test_simple_fill();
            test_partial_fill();
            test_multi_level_sweep();
            test_cancel_order();
            test_price_time_priority();
            test_invariants();
            test_replay_determinism();
            test_empty_book();
            test_crossed_order();
            test_rejects_invalid_input();
            test_rejects_duplicate_id();
            test_command_event_sequences();
            test_command_sequence_rules();
            test_pool_exhaustion_is_deterministic();
            test_price_ladder_fallback();
            test_high_frequency_cancel_reuse();
            test_event_reserve_configuration();
            test_limit_level_pool_lifecycle();
            test_instrument_rules();
            test_disk_log_roundtrip();
            test_rejects_corrupt_log();
            
            std::cout << "\n✅ All tests passed!\n";
        } catch (const std::exception& e) {
            std::cerr << "\n❌ Test failed with exception: " << e.what() << "\n";
            exit(1);
        }
    }
    
private:
    // Helper: Compare internal price with expected double (approximate match not needed for integers)
    static bool eq_price(Price p, double expected) {
        return p.get() == static_cast<int64_t>(expected * PRICE_SCALE);
    }

    static void test_simple_fill() {
        std::cout << "Test 1: Simple Fill... ";
        OrderBook book;
        
        // Sell 10 @ 100.0
        book.process_new_order(OrderId(1), Side::SELL, from_double(100.0), Quantity(10));
        // Buy 10 @ 100.0 -> Match
        book.process_new_order(OrderId(2), Side::BUY, from_double(100.0), Quantity(10));
        
        TEST_ASSERT(!book.best_bid().has_value());
        TEST_ASSERT(!book.best_ask().has_value());
        std::cout << "Passed\n";
    }
    
    static void test_partial_fill() {
        std::cout << "Test 2: Partial Fill... ";
        OrderBook book;
        
        // Sell 10 @ 100.0
        book.process_new_order(OrderId(1), Side::SELL, from_double(100.0), Quantity(10));
        // Buy 5 @ 100.0 -> Partial Match
        book.process_new_order(OrderId(2), Side::BUY, from_double(100.0), Quantity(5));
        
        // Remaining Sell 5 @ 100.0
        TEST_ASSERT(book.best_ask().has_value());
        TEST_ASSERT(eq_price(*book.best_ask(), 100.0));
        
        // Check event log for Trade
        const auto& log = book.get_event_log();
        TEST_ASSERT(!log.empty());
        bool found_trade = false;
        for(const auto& evt : log) {
            if (std::holds_alternative<TradeEvent>(evt)) {
                const auto& trade = std::get<TradeEvent>(evt);
                if (trade.quantity.get() == 5) found_trade = true;
            }
        }
        TEST_ASSERT(found_trade);
        std::cout << "Passed\n";
    }
    
    static void test_multi_level_sweep() {
        std::cout << "Test 3: Multi-Level Sweep... ";
        OrderBook book;
        
        book.process_new_order(OrderId(1), Side::SELL, from_double(100.0), Quantity(10));
        book.process_new_order(OrderId(2), Side::SELL, from_double(101.0), Quantity(10));
        book.process_new_order(OrderId(3), Side::SELL, from_double(102.0), Quantity(10));
        
        // Buy 25 @ 105.0 -> Sweeps 100.0 and 101.0, eats 5 of 102.0
        book.process_new_order(OrderId(4), Side::BUY, from_double(105.0), Quantity(25));
        
        TEST_ASSERT(book.best_ask().has_value());
        TEST_ASSERT(eq_price(*book.best_ask(), 102.0));
        std::cout << "Passed\n";
    }
    
    static void test_cancel_order() {
        std::cout << "Test 4: Cancel Order... ";
        OrderBook book;
        
        book.process_new_order(OrderId(1), Side::SELL, from_double(100.0), Quantity(10));
        book.process_cancel(OrderId(1));
        
        TEST_ASSERT(!book.best_ask().has_value());
        
        // Verify cancel event
        const auto& log = book.get_event_log();
        TEST_ASSERT(std::holds_alternative<OrderCancelledEvent>(log.back()));
        std::cout << "Passed\n";
    }
    
    static void test_price_time_priority() {
        std::cout << "Test 5: Price-Time Priority... ";
        OrderBook book;
        
        // Sell orders at same price
        book.process_new_order(OrderId(1), Side::SELL, from_double(100.0), Quantity(10));
        book.process_new_order(OrderId(2), Side::SELL, from_double(100.0), Quantity(10));
        
        // Buy 5 -> Should match with Order 1 (First in)
        book.process_new_order(OrderId(3), Side::BUY, from_double(100.0), Quantity(5));
        
        const auto& log = book.get_event_log();
        TEST_ASSERT(!log.empty());
        
        // Verify last event is Trade matching Order 1
        const EngineEvent& last_evt = log.back();
        const TradeEvent* trade = std::get_if<TradeEvent>(&last_evt);
        
        TEST_ASSERT(trade != nullptr);
        TEST_ASSERT(trade->passive_order_id.get() == 1);
        TEST_ASSERT(trade->aggressive_order_id.get() == 3);
        std::cout << "Passed\n";
    }
    
    static void test_invariants() {
        std::cout << "Test 6: Invariants... ";
        OrderBook book;
        
        book.process_new_order(OrderId(1), Side::BUY, from_double(99.0), Quantity(10));
        book.process_new_order(OrderId(2), Side::SELL, from_double(101.0), Quantity(10));
        
        // Manual invariant check: Bid < Ask
        TEST_ASSERT(book.best_bid().has_value());
        TEST_ASSERT(book.best_ask().has_value());
        TEST_ASSERT(book.best_bid()->get() < book.best_ask()->get());
        TEST_ASSERT(book.check_invariants());
        std::cout << "Passed\n";
    }
    
    static void test_replay_determinism() {
        std::cout << "Test 7: Replay Determinism... ";
        OrderBook book1;

        const std::vector<Command> commands{
            NewOrderCommand(CommandSequence(1), OrderId(1), Side::SELL, from_double(100.0), Quantity(10)),
            NewOrderCommand(CommandSequence(2), OrderId(2), Side::BUY, from_double(100.0), Quantity(5)),
            NewOrderCommand(CommandSequence(3), OrderId(3), Side::SELL, from_double(101.0), Quantity(10))
        };
        for (const Command& command : commands) book1.process(command);
        
        // Replay
        OrderBook book2 = ReplayEngine::replay_commands(commands);
        
        // Verify state equality
        // 1. Check Best Ask
        TEST_ASSERT(book1.best_ask().has_value() == book2.best_ask().has_value());
        if (book1.best_ask().has_value()) {
            TEST_ASSERT(book1.best_ask()->get() == book2.best_ask()->get());
        }
        
        // 2. Check Best Bid
        TEST_ASSERT(book1.best_bid().has_value() == book2.best_bid().has_value());
        TEST_ASSERT(book1.state_hash() == book2.state_hash());
        
        std::cout << "Passed\n";
    }
    
    static void test_empty_book() {
        std::cout << "Test 8: Empty Book Edge Case... ";
        OrderBook book;
        
        TEST_ASSERT(!book.best_bid().has_value());
        TEST_ASSERT(!book.best_ask().has_value());
        
        book.process_cancel(OrderId(999)); // Should not crash
        std::cout << "Passed\n";
    }
    
    static void test_crossed_order() {
        std::cout << "Test 9: Crossed Order Prevention... ";
        OrderBook book;
        
        book.process_new_order(OrderId(1), Side::BUY, from_double(100.0), Quantity(10));
        book.process_new_order(OrderId(2), Side::SELL, from_double(101.0), Quantity(10));
        
        // Aggressive Buy @ 102.0 (Crosses 101.0) -> Match
        book.process_new_order(OrderId(3), Side::BUY, from_double(102.0), Quantity(10));
        
        // After match, book should be uncrossed
        if (book.best_bid().has_value() && book.best_ask().has_value()) {
            TEST_ASSERT(book.best_bid()->get() < book.best_ask()->get());
        }
        std::cout << "Passed\n";
    }

    static void test_rejects_invalid_input() {
        std::cout << "Test 10: Input Validation... ";
        OrderBook book(16);
        book.process_new_order(OrderId(10), Side::BUY, Price(0), Quantity(1));
        book.process_new_order(OrderId(11), Side::BUY, from_double(100.0), Quantity(0));
        TEST_ASSERT(!book.best_bid().has_value());
        TEST_ASSERT(book.get_event_log().size() == 2);
        TEST_ASSERT(std::holds_alternative<OrderRejectedEvent>(book.get_event_log()[0]));
        TEST_ASSERT(std::get<OrderRejectedEvent>(book.get_event_log()[0]).reason == RejectReason::INVALID_PRICE);
        std::cout << "Passed\n";
    }

    static void test_rejects_duplicate_id() {
        std::cout << "Test 11: Duplicate Order ID... ";
        TEST_ASSERT(sizeof(void*) != 8 || FixedOrderIndex::slot_size_bytes() == 16);
        OrderBook book(16);
        book.process_new_order(OrderId(1), Side::BUY, from_double(100.0), Quantity(10));
        book.process_new_order(OrderId(1), Side::SELL, from_double(101.0), Quantity(10));
        // Duplicate detection remains the first stateful rejection even when
        // later fields are also invalid; the reservation must not be committed.
        book.process_new_order(OrderId(1), Side::SELL, Price(0), Quantity(0));
        book.process_cancel(OrderId(1));
        TEST_ASSERT(!book.best_bid().has_value());
        TEST_ASSERT(std::holds_alternative<OrderRejectedEvent>(book.get_event_log()[1]));
        TEST_ASSERT(std::get<OrderRejectedEvent>(book.get_event_log()[1]).reason == RejectReason::DUPLICATE_ORDER_ID);
        TEST_ASSERT(std::get<OrderRejectedEvent>(book.get_event_log()[2]).reason == RejectReason::DUPLICATE_ORDER_ID);
        std::cout << "Passed\n";
    }

    static void test_pool_exhaustion_is_deterministic() {
        std::cout << "Test 12: Pool Exhaustion... ";
        OrderBook book(1);
        const std::vector<Command> commands{
            NewOrderCommand(CommandSequence(1), OrderId(1), Side::BUY, from_double(100.0), Quantity(10)),
            NewOrderCommand(CommandSequence(2), OrderId(2), Side::BUY, from_double(99.0), Quantity(10))
        };
        for (const Command& command : commands) book.process(command);
        const auto& log = book.get_event_log();
        TEST_ASSERT(log.size() == 2);
        TEST_ASSERT(std::holds_alternative<OrderRejectedEvent>(log.back()));
        TEST_ASSERT(std::get<OrderRejectedEvent>(log.back()).reason == RejectReason::POOL_EXHAUSTED);
        TEST_ASSERT(book.state_hash() ==
                    ReplayEngine::replay_commands(commands, {}, {}, 1).state_hash());
        std::cout << "Passed\n";
    }

    static void test_command_event_sequences() {
        std::cout << "Test 12: Command/EngineEvent Sequences... ";

        {
            OrderBook book(16);
            const ProcessResult result = book.process(NewOrderCommand(
                CommandSequence(1), OrderId(1), Side::SELL,
                from_double(100.0), Quantity(10)));
            TEST_ASSERT(result.applied());
            TEST_ASSERT(result.event_begin == 0 && result.event_count == 1);
            const auto* rested = std::get_if<OrderRestedEvent>(&book.engine_events()[0]);
            TEST_ASSERT(rested != nullptr);
            TEST_ASSERT(rested->order_id.get() == 1);
            TEST_ASSERT(rested->original_quantity.get() == 10);
            TEST_ASSERT(rested->remaining_quantity.get() == 10);
        }

        {
            OrderBook book(16);
            book.process(NewOrderCommand(CommandSequence(1), OrderId(1), Side::SELL,
                                         from_double(100.0), Quantity(10)));
            book.process(NewOrderCommand(CommandSequence(2), OrderId(2), Side::SELL,
                                         from_double(101.0), Quantity(10)));
            book.process(NewOrderCommand(CommandSequence(3), OrderId(3), Side::SELL,
                                         from_double(102.0), Quantity(10)));
            const ProcessResult result = book.process(NewOrderCommand(
                CommandSequence(4), OrderId(4), Side::BUY,
                from_double(105.0), Quantity(25)));
            TEST_ASSERT(result.applied() && result.event_count == 3);
            for (size_t i = 0; i < result.event_count; ++i) {
                const auto* trade = std::get_if<TradeEvent>(
                    &book.engine_events()[result.event_begin + i]);
                TEST_ASSERT(trade != nullptr);
                TEST_ASSERT(trade->command_sequence.get() == 4);
                TEST_ASSERT(trade->event_index.get() == i);
                TEST_ASSERT(trade->passive_order_id.get() == i + 1);
                TEST_ASSERT(trade->aggressive_order_id.get() == 4);
            }
        }

        {
            OrderBook book(16);
            book.process(NewOrderCommand(CommandSequence(1), OrderId(1), Side::SELL,
                                         from_double(100.0), Quantity(10)));
            const ProcessResult result = book.process(NewOrderCommand(
                CommandSequence(2), OrderId(2), Side::BUY,
                from_double(100.0), Quantity(15)));
            TEST_ASSERT(result.applied() && result.event_count == 2);
            TEST_ASSERT(std::holds_alternative<TradeEvent>(
                book.engine_events()[result.event_begin]));
            TEST_ASSERT(get_event_index(book.engine_events()[result.event_begin]).get() == 0);
            const auto* rested = std::get_if<OrderRestedEvent>(
                &book.engine_events()[result.event_begin + 1]);
            TEST_ASSERT(rested != nullptr);
            TEST_ASSERT(rested->order_id.get() == 2);
            TEST_ASSERT(rested->remaining_quantity.get() == 5);
            TEST_ASSERT(rested->event_index.get() == 1);
        }

        {
            OrderBook book(16);
            book.process(NewOrderCommand(CommandSequence(1), OrderId(1), Side::BUY,
                                         from_double(100.0), Quantity(10)));
            const ProcessResult duplicate = book.process(NewOrderCommand(
                CommandSequence(2), OrderId(1), Side::SELL,
                from_double(101.0), Quantity(10)));
            TEST_ASSERT(duplicate.status == ProcessStatus::REJECTED);
            TEST_ASSERT(duplicate.event_count == 1);
            const auto* rejected = std::get_if<OrderRejectedEvent>(
                &book.engine_events()[duplicate.event_begin]);
            TEST_ASSERT(rejected != nullptr);
            TEST_ASSERT(rejected->command_type == CommandType::NEW_ORDER);
            TEST_ASSERT(rejected->reason == RejectReason::DUPLICATE_ORDER_ID);

            const ProcessResult missing = book.process(
                CancelOrderCommand(CommandSequence(3), OrderId(999)));
            TEST_ASSERT(missing.status == ProcessStatus::REJECTED);
            TEST_ASSERT(missing.event_count == 1);
            rejected = std::get_if<OrderRejectedEvent>(
                &book.engine_events()[missing.event_begin]);
            TEST_ASSERT(rejected != nullptr);
            TEST_ASSERT(rejected->command_type == CommandType::CANCEL_ORDER);
            TEST_ASSERT(rejected->reason == RejectReason::ORDER_NOT_FOUND);
        }

        std::cout << "Passed\n";
    }

    static void test_command_sequence_rules() {
        std::cout << "Test: Deterministic Command Sequence... ";
        OrderBook book(16);

        const ProcessResult first = book.process(NewOrderCommand(
            CommandSequence(10), OrderId(1), Side::SELL,
            from_double(100.0), Quantity(10)));
        TEST_ASSERT(first.applied());
        TEST_ASSERT(book.last_applied_command_sequence().get() == 10);
        const auto* first_rested = std::get_if<OrderRestedEvent>(
            &book.engine_events()[first.event_begin]);
        TEST_ASSERT(first_rested != nullptr);
        TEST_ASSERT(first_rested->priority_sequence.get() == 10);

        const uint64_t before_invalid_sequence = book.state_hash();
        const ProcessResult duplicate_sequence = book.process(NewOrderCommand(
            CommandSequence(10), OrderId(2), Side::SELL,
            from_double(100.0), Quantity(10)));
        TEST_ASSERT(duplicate_sequence.status == ProcessStatus::SEQUENCE_REJECTED);
        TEST_ASSERT(book.state_hash() == before_invalid_sequence);
        const auto* sequence_reject = std::get_if<OrderRejectedEvent>(
            &book.engine_events()[duplicate_sequence.event_begin]);
        TEST_ASSERT(sequence_reject != nullptr);
        TEST_ASSERT(sequence_reject->reason == RejectReason::INVALID_COMMAND_SEQUENCE);
        TEST_ASSERT(sequence_reject->command_sequence.get() == 10);
        TEST_ASSERT(sequence_reject->event_index.get() == 0);

        const ProcessResult backwards = book.process(
            CancelOrderCommand(CommandSequence(9), OrderId(1)));
        TEST_ASSERT(backwards.status == ProcessStatus::SEQUENCE_REJECTED);
        TEST_ASSERT(book.state_hash() == before_invalid_sequence);

        const ProcessResult zero = book.process(
            CancelOrderCommand(CommandSequence(0), OrderId(1)));
        TEST_ASSERT(zero.status == ProcessStatus::SEQUENCE_REJECTED);
        TEST_ASSERT(book.state_hash() == before_invalid_sequence);

        const ProcessResult business_reject = book.process(NewOrderCommand(
            CommandSequence(20), OrderId(99), Side::BUY, Price(0), Quantity(1)));
        TEST_ASSERT(business_reject.status == ProcessStatus::REJECTED);
        TEST_ASSERT(book.last_applied_command_sequence().get() == 20);
        TEST_ASSERT(get_event_index(book.engine_events()[business_reject.event_begin]).get() == 0);

        const ProcessResult second = book.process(NewOrderCommand(
            CommandSequence(30), OrderId(2), Side::SELL,
            from_double(100.0), Quantity(10)));
        TEST_ASSERT(second.applied());
        const auto* second_rested = std::get_if<OrderRestedEvent>(
            &book.engine_events()[second.event_begin]);
        TEST_ASSERT(second_rested != nullptr);
        TEST_ASSERT(second_rested->priority_sequence.get() == 30);

        const ProcessResult sweep = book.process(NewOrderCommand(
            CommandSequence(40), OrderId(3), Side::BUY,
            from_double(100.0), Quantity(20)));
        TEST_ASSERT(sweep.applied() && sweep.event_count == 2);
        for (size_t i = 0; i < sweep.event_count; ++i) {
            const auto* trade = std::get_if<TradeEvent>(
                &book.engine_events()[sweep.event_begin + i]);
            TEST_ASSERT(trade != nullptr);
            TEST_ASSERT(trade->event_index.get() == i);
            TEST_ASSERT(trade->passive_order_id.get() == i + 1);
        }
        TEST_ASSERT(book.last_applied_command_sequence().get() == 40);
        TEST_ASSERT(book.check_invariants());
        std::cout << "Passed\n";
    }

    static void test_price_ladder_fallback() {
        std::cout << "Test 13: Price Ladder Fallback... ";
        PriceLadderConfig config{1000000, 1010000, 100}; // 100.00..101.00, tick 0.01
        OrderBook book(32, true, config);

        // Inside the configured range: dense ladder path.
        book.process_new_order(OrderId(1), Side::SELL, from_double(100.50), Quantity(10));
        // Outside the range: std::map fallback path.
        book.process_new_order(OrderId(2), Side::SELL, from_double(200.00), Quantity(10));
        TEST_ASSERT(book.best_ask().has_value());
        TEST_ASSERT(eq_price(*book.best_ask(), 100.50));

        book.process_new_order(OrderId(3), Side::BUY, from_double(100.50), Quantity(10));
        TEST_ASSERT(book.best_ask().has_value());
        TEST_ASSERT(eq_price(*book.best_ask(), 200.00));
        std::cout << "Passed\n";
    }

    static void test_high_frequency_cancel_reuse() {
        std::cout << "Test 14: High-Frequency Cancel/Reuse... ";
        OrderBook book(32, false);
        // Repeatedly fill and release the same number of slots. This exercises
        // index deletion/cluster repair and price-level destruction without
        // allowing stale IDs or tombstones to accumulate.
        for (uint64_t round = 0; round < 200; ++round) {
            const uint64_t base = round * 32 + 1;
            for (uint64_t i = 0; i < 16; ++i) {
                book.process_new_order(OrderId(base + i), Side::BUY,
                                       from_double(100.0 + static_cast<double>(i)), Quantity(1));
            }
            for (uint64_t i = 0; i < 16; ++i) {
                book.process_cancel(OrderId(base + i));
            }
            TEST_ASSERT(!book.best_bid().has_value());
            TEST_ASSERT(book.available_level_slots() == book.level_pool_capacity());
        }

        // capacity=4 creates a 16-slot table. Because the Fibonacci multiplier
        // is odd, IDs separated by 16 share the same masked start slot.
        FixedOrderIndex collision_index(4);
        Order first(OrderId(1), PrioritySequence(1), Side::BUY, Price(100), Quantity(1));
        Order middle(OrderId(17), PrioritySequence(2), Side::BUY, Price(100), Quantity(1));
        Order last(OrderId(33), PrioritySequence(3), Side::BUY, Price(100), Quantity(1));
        const auto first_slot = collision_index.prepare_insert(first.id.get());
        TEST_ASSERT(first_slot.status == FixedOrderIndex::InsertStatus::AVAILABLE);
        TEST_ASSERT(collision_index.commit_insert(first_slot, first.id.get(), &first));
        const auto middle_slot = collision_index.prepare_insert(middle.id.get());
        TEST_ASSERT(middle_slot.status == FixedOrderIndex::InsertStatus::AVAILABLE);
        TEST_ASSERT(collision_index.commit_insert(middle_slot, middle.id.get(), &middle));
        const auto last_slot = collision_index.prepare_insert(last.id.get());
        TEST_ASSERT(last_slot.status == FixedOrderIndex::InsertStatus::AVAILABLE);
        TEST_ASSERT(collision_index.commit_insert(last_slot, last.id.get(), &last));
        TEST_ASSERT(collision_index.prepare_insert(middle.id.get()).status ==
                    FixedOrderIndex::InsertStatus::DUPLICATE);
        TEST_ASSERT(collision_index.erase(middle.id.get()));
        TEST_ASSERT(collision_index.find(first.id.get()) == &first);
        TEST_ASSERT(collision_index.find(middle.id.get()) == nullptr);
        TEST_ASSERT(collision_index.find(last.id.get()) == &last);
        std::cout << "Passed\n";
    }

    static void test_event_reserve_configuration() {
        std::cout << "Test 15: Event Reserve Configuration... ";
        OrderBook compact(32, true, {}, 1);
        OrderBook replay_capture(32, true, {}, 2);
        TEST_ASSERT(compact.event_log_capacity() >= 32);
        TEST_ASSERT(replay_capture.event_log_capacity() >= 64);
        std::cout << "Passed\n";
    }

    static void test_limit_level_pool_lifecycle() {
        std::cout << "Test 16: Limit-Level Pool Lifecycle... ";
        LimitLevelPool pool(2);
        LimitLevel* first = pool.acquire(from_double(100.0));
        LimitLevel* second = pool.acquire(from_double(101.0));
        TEST_ASSERT(first != nullptr && second != nullptr);
        TEST_ASSERT(pool.available() == 0);
        TEST_ASSERT(pool.acquire(from_double(102.0)) == nullptr);
        pool.release(first);
        TEST_ASSERT(pool.available() == 1);
        LimitLevel* reused = pool.acquire(from_double(102.0));
        TEST_ASSERT(reused == first);
        pool.release(second);
        pool.release(reused);
        pool.release(reused); // double release must be ignored
        pool.release(reinterpret_cast<LimitLevel*>(static_cast<uintptr_t>(1)));
        TEST_ASSERT(pool.available() == 2);
        std::cout << "Passed\n";
    }

    static void test_instrument_rules() {
        std::cout << "Test 17: Instrument Rules... ";
        InstrumentConfig rules;
        rules.tick_size = 100;
        rules.lot_size = 10;
        rules.min_price = 1000000;
        rules.max_price = 2000000;
        rules.max_order_quantity = 100;
        OrderBook book(16, true, {}, 2, rules);
        const std::vector<Command> commands{
            NewOrderCommand(CommandSequence(1), OrderId(1), Side::BUY, Price(1000001), Quantity(10)),
            NewOrderCommand(CommandSequence(2), OrderId(2), Side::BUY, Price(999900), Quantity(10)),
            NewOrderCommand(CommandSequence(3), OrderId(3), Side::BUY, Price(1000000), Quantity(11)),
            NewOrderCommand(CommandSequence(4), OrderId(4), Side::BUY, Price(1000000), Quantity(110)),
            NewOrderCommand(CommandSequence(5), OrderId(5), Side::BUY, Price(1000000), Quantity(10))
        };
        for (const Command& command : commands) book.process(command);
        const auto& log = book.get_event_log();
        TEST_ASSERT(std::get<OrderRejectedEvent>(log[0]).reason == RejectReason::OFF_TICK_PRICE);
        TEST_ASSERT(std::get<OrderRejectedEvent>(log[1]).reason == RejectReason::PRICE_OUT_OF_RANGE);
        TEST_ASSERT(std::get<OrderRejectedEvent>(log[2]).reason == RejectReason::INVALID_LOT_SIZE);
        TEST_ASSERT(std::get<OrderRejectedEvent>(log[3]).reason == RejectReason::QUANTITY_LIMIT);
        TEST_ASSERT(std::holds_alternative<OrderRestedEvent>(log[4]));
        TEST_ASSERT(book.state_hash() == ReplayEngine::replay_commands(commands, rules).state_hash());
        std::cout << "Passed\n";
    }

    static void test_disk_log_roundtrip() {
        std::cout << "Test 18: Disk Log Roundtrip... ";
        const auto path = std::filesystem::temp_directory_path() / "matching_engine_roundtrip.csv";
        OrderBook original(32);
        const std::vector<Command> commands{
            NewOrderCommand(CommandSequence(1), OrderId(99), Side::BUY, Price(1000001), Quantity(10)),
            NewOrderCommand(CommandSequence(2), OrderId(1), Side::SELL, Price(1000000), Quantity(10)),
            NewOrderCommand(CommandSequence(3), OrderId(2), Side::BUY, Price(1000000), Quantity(10)),
            CancelOrderCommand(CommandSequence(4), OrderId(888))
        };
        for (const Command& command : commands) original.process(command);
        ReplayEngine::save_log(original.get_event_log(), path.string());
        const auto loaded = ReplayEngine::load_log(path.string());
        TEST_ASSERT(loaded.size() == original.get_event_log().size());
        char lhs[256];
        char rhs[256];
        for (size_t i = 0; i < loaded.size(); ++i) {
            event_to_buffer(original.get_event_log()[i], lhs, sizeof(lhs));
            event_to_buffer(loaded[i], rhs, sizeof(rhs));
            TEST_ASSERT(std::string(lhs) == std::string(rhs));
        }
        TEST_ASSERT(original.state_hash() == ReplayEngine::replay_commands(commands).state_hash());
        std::filesystem::remove(path);
        std::cout << "Passed\n";
    }

    static void test_rejects_corrupt_log() {
        std::cout << "Test 19: Corrupt Log Rejection... ";
        const auto path = std::filesystem::temp_directory_path() / "matching_engine_corrupt.csv";
        {
            std::ofstream file(path);
            file << "NEW_ORDER,1,1,HOLD,1000000,10\n";
        }
        bool rejected = false;
        try {
            (void)ReplayEngine::load_log(path.string());
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        std::filesystem::remove(path);
        TEST_ASSERT(rejected);
        std::cout << "Passed\n";
    }
};

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "╔════════════════════════════════════════════════════════════╗\n";
    std::cout << "║              UNIT TEST SUITE                               ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════╝\n\n";
    
    try {
        TestSuite::run_all_tests();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "CRITICAL ERROR: " << e.what() << "\n";
        return 1;
    }
}
