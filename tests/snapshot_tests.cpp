#include "../src/orderbook.hpp"
#include "../src/snapshot_recovery.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

#define TEST_ASSERT(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "Snapshot assertion failed: " << #condition \
                      << " at " << __FILE__ << ':' << __LINE__ << '\n'; \
            throw std::runtime_error("Snapshot test assertion failed"); \
        } \
    } while (false)

namespace {

std::vector<Command> prefix_commands() {
    return {
        NewOrderCommand(CommandSequence(10), OrderId(1), Side::SELL,
                        from_double(101.0), Quantity(10)),
        NewOrderCommand(CommandSequence(20), OrderId(2), Side::SELL,
                        from_double(101.0), Quantity(15)),
        NewOrderCommand(CommandSequence(30), OrderId(3), Side::BUY,
                        from_double(99.0), Quantity(8)),
        NewOrderCommand(CommandSequence(40), OrderId(4), Side::BUY,
                        from_double(101.0), Quantity(12)),
        NewOrderCommand(CommandSequence(50), OrderId(5), Side::SELL,
                        from_double(102.0), Quantity(7))
    };
}

std::vector<Command> suffix_commands() {
    return {
        NewOrderCommand(CommandSequence(60), OrderId(6), Side::BUY,
                        from_double(103.0), Quantity(18)),
        CancelOrderCommand(CommandSequence(70), OrderId(3)),
        NewOrderCommand(CommandSequence(80), OrderId(7), Side::BUY,
                        from_double(100.0), Quantity(4))
    };
}

void execute(OrderBook& book, const std::vector<Command>& commands) {
    for (const Command& command : commands) {
        (void)book.process(command);
    }
}

OrderBookSnapshot populated_snapshot() {
    OrderBook book(64);
    execute(book, prefix_commands());
    TEST_ASSERT(book.check_invariants());
    return book.create_snapshot();
}

void expect_failure(const OrderBookSnapshot& snapshot, size_t capacity,
                    SnapshotRestoreError expected_error) {
    SnapshotRestoreResult result = SnapshotRecovery::restore(snapshot, capacity);
    TEST_ASSERT(!result.success());
    TEST_ASSERT(result.book == nullptr);
    TEST_ASSERT(result.error == expected_error);
}

void test_empty_snapshot() {
    OrderBook empty(16);
    const OrderBookSnapshot snapshot = empty.create_snapshot();
    SnapshotRestoreResult result = SnapshotRecovery::restore(snapshot, 16);
    TEST_ASSERT(result.success());
    TEST_ASSERT(result.book->capture_state() == empty.capture_state());
    TEST_ASSERT(result.book->state_hash() == empty.state_hash());
    TEST_ASSERT(result.book->engine_events().empty());
}

void test_snapshot_state_recovery() {
    OrderBook original(64);
    execute(original, prefix_commands());
    const OrderBookSnapshot snapshot = original.create_snapshot();

    TEST_ASSERT(snapshot.snapshot_version == CURRENT_SNAPSHOT_VERSION);
    TEST_ASSERT(snapshot.last_applied_command_sequence.get() == 50);
    TEST_ASSERT(snapshot.instrument_config == original.instrument_config());
    TEST_ASSERT(snapshot.price_ladder_config == original.price_ladder_config());
    TEST_ASSERT(snapshot.active_orders.size() == original.active_order_count());

    SnapshotRestoreResult result = SnapshotRecovery::restore(snapshot, 64);
    TEST_ASSERT(result.success());
    TEST_ASSERT(result.book->check_invariants());
    TEST_ASSERT(result.book->capture_state() == original.capture_state());
    TEST_ASSERT(result.book->state_hash() == original.state_hash());
    TEST_ASSERT(result.book->best_bid() == original.best_bid());
    TEST_ASSERT(result.book->best_ask() == original.best_ask());
    TEST_ASSERT(result.book->last_applied_command_sequence() ==
                original.last_applied_command_sequence());
    TEST_ASSERT(result.book->engine_events().empty());
}

void test_snapshot_continue_execution() {
    OrderBook uninterrupted(64);
    const std::vector<Command> prefix = prefix_commands();
    const std::vector<Command> suffix = suffix_commands();
    execute(uninterrupted, prefix);
    const size_t prefix_event_count = uninterrupted.engine_events().size();
    const OrderBookSnapshot snapshot = uninterrupted.create_snapshot();

    SnapshotRestoreResult result = SnapshotRecovery::restore(snapshot, 64);
    TEST_ASSERT(result.success());
    execute(uninterrupted, suffix);
    execute(*result.book, suffix);

    TEST_ASSERT(result.book->capture_state() == uninterrupted.capture_state());
    TEST_ASSERT(result.book->state_hash() == uninterrupted.state_hash());
    TEST_ASSERT(result.book->last_applied_command_sequence().get() == 80);
    TEST_ASSERT(result.book->check_invariants());

    const auto& complete_events = uninterrupted.engine_events();
    const auto& recovered_suffix_events = result.book->engine_events();
    TEST_ASSERT(complete_events.size() - prefix_event_count ==
                recovered_suffix_events.size());
    for (size_t i = 0; i < recovered_suffix_events.size(); ++i) {
        TEST_ASSERT(complete_events[prefix_event_count + i] ==
                    recovered_suffix_events[i]);
    }
}

void test_invalid_snapshots_are_atomic() {
    const OrderBookSnapshot valid = populated_snapshot();
    TEST_ASSERT(valid.active_orders.size() >= 2);

    OrderBookSnapshot bad_version = valid;
    bad_version.snapshot_version = CURRENT_SNAPSHOT_VERSION + 1;
    expect_failure(bad_version, 64, SnapshotRestoreError::UNSUPPORTED_VERSION);

    expect_failure(valid, valid.active_orders.size() - 1,
                   SnapshotRestoreError::CAPACITY_EXCEEDED);

    OrderBookSnapshot duplicate_id = valid;
    duplicate_id.active_orders[1].order_id = duplicate_id.active_orders[0].order_id;
    expect_failure(duplicate_id, 64, SnapshotRestoreError::DUPLICATE_ORDER_ID);

    OrderBookSnapshot duplicate_priority = valid;
    duplicate_priority.active_orders[1].priority_sequence =
        duplicate_priority.active_orders[0].priority_sequence;
    expect_failure(duplicate_priority, 64,
                   SnapshotRestoreError::DUPLICATE_PRIORITY_SEQUENCE);

    OrderBookSnapshot future_priority = valid;
    future_priority.active_orders[0].priority_sequence =
        PrioritySequence(valid.last_applied_command_sequence.get() + 1);
    expect_failure(future_priority, 64,
                   SnapshotRestoreError::INVALID_PRIORITY_SEQUENCE);

    OrderBookSnapshot zero_remaining = valid;
    zero_remaining.active_orders[0].remaining_quantity = Quantity(0);
    expect_failure(zero_remaining, 64, SnapshotRestoreError::INVALID_QUANTITY);

    OrderBookSnapshot bad_config = valid;
    bad_config.instrument_config.tick_size = 0;
    expect_failure(bad_config, 64,
                   SnapshotRestoreError::INVALID_INSTRUMENT_CONFIG);

    OrderBookSnapshot crossed = valid;
    int64_t best_ask = std::numeric_limits<int64_t>::max();
    for (const SnapshotOrder& order : crossed.active_orders) {
        if (order.side == Side::SELL && order.price.get() < best_ask) {
            best_ask = order.price.get();
        }
    }
    for (SnapshotOrder& order : crossed.active_orders) {
        if (order.side == Side::BUY) {
            order.price = Price(best_ask);
            break;
        }
    }
    expect_failure(crossed, 64, SnapshotRestoreError::CROSSED_BOOK);
}

} // namespace

int main() {
    try {
        test_empty_snapshot();
        test_snapshot_state_recovery();
        test_snapshot_continue_execution();
        test_invalid_snapshots_are_atomic();
        std::cout << "All snapshot tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
