#include "journal.hpp"
#include "orderbook.hpp"
#include "reference_engine.hpp"
#include "replay.hpp"
#include "replay_v2.hpp"
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

#define TEST_ASSERT(x) do { if (!(x)) throw std::runtime_error("assertion failed: " #x); } while(false)

namespace {

constexpr uint64_t MAX_VOLUME = std::numeric_limits<uint64_t>::max();

// Compare the independent reference after every boundary command, not just the
// final hash. Keep the event sink unbounded so these tests isolate book rules.
struct PairedBook {
    OrderBook optimized;
    ReferenceEngine reference;
    VectorEventSink actual;
    std::vector<EngineEvent> expected;
    uint64_t sequence = 0;

    explicit PairedBook(size_t capacity = 8, InstrumentConfig rules = {})
        : optimized(capacity, true, {}, 2, rules), reference(capacity, rules) {}

    ProcessResult process(const Command& command) {
        actual.clear();
        const auto result = optimized.process(command, actual);
        const auto reference_status = reference.process(command, expected);
        TEST_ASSERT(result.status == reference_status);
        TEST_ASSERT(result.event_output_complete);
        TEST_ASSERT(actual.events() == expected);
        TEST_ASSERT(optimized.capture_state() == reference.capture_state());
        TEST_ASSERT(optimized.check_invariants());
        return result;
    }

    ProcessResult insert(uint64_t id, Side side, int64_t price, uint64_t volume,
                         TimeInForce tif = TimeInForce::GTC) {
        return process(NewOrderCommand(CommandSequence(++sequence), OrderId(id),
            side, Price(price), Quantity(volume), tif));
    }

    ProcessResult cancel(uint64_t id) {
        return process(CancelOrderCommand(CommandSequence(++sequence), OrderId(id)));
    }

    void rejected(const ProcessResult& result, RejectReason reason) const {
        TEST_ASSERT(result.status == ProcessStatus::REJECTED);
        TEST_ASSERT(actual.events().size() == 1);
        TEST_ASSERT(std::get<OrderRejectedEvent>(actual.events().front()).reason == reason);
    }
};

Side opposite(Side side) {
    return side == Side::BUY ? Side::SELL : Side::BUY;
}

const BookLevelState& same_side_level(const OrderBookState& state, Side side) {
    return side == Side::BUY ? state.bid_levels.at(0) : state.ask_levels.at(0);
}

void test_level_volume_limit_and_atomic_rejection() {
    for (Side side : {Side::BUY, Side::SELL}) {
        // Exercise the dense ladder and the ordered-map fallback.
        for (int64_t price : {int64_t(1'000'000), int64_t(100)}) {
            for (TimeInForce tif : {TimeInForce::GTC, TimeInForce::POST_ONLY}) {
                PairedBook books(3);
                TEST_ASSERT(books.insert(1, side, price, MAX_VOLUME - 1).applied());
                TEST_ASSERT(books.insert(2, side, price, 1).applied());
                auto before = books.optimized.capture_state();
                TEST_ASSERT(same_side_level(before, side).total_volume == Quantity(MAX_VOLUME));
                const auto capacity_before = books.optimized.capacity_metrics();
                const size_t slots_before = books.optimized.available_level_slots();

                const auto result = books.insert(3, side, price, 1, tif);
                books.rejected(result, RejectReason::LEVEL_VOLUME_OVERFLOW);
                // A business rejection consumes the sequence, but no book state.
                before.last_applied_command_sequence = CommandSequence(books.sequence);
                TEST_ASSERT(books.optimized.capture_state() == before);
                const auto capacity_after = books.optimized.capacity_metrics();
                TEST_ASSERT(capacity_after.active_orders == capacity_before.active_orders);
                TEST_ASSERT(capacity_after.active_levels == capacity_before.active_levels);
                TEST_ASSERT(capacity_after.index_load == capacity_before.index_load);
                TEST_ASSERT(capacity_after.peak_active_orders == capacity_before.peak_active_orders);
                TEST_ASSERT(books.optimized.available_level_slots() == slots_before);

                // Reuse the rejected ID and the last order slot at a new level.
                TEST_ASSERT(books.insert(3, side, price + 100, 1).applied());
                TEST_ASSERT(books.optimized.active_order_count() == 3);
            }
        }
    }
}

void test_cancel_releases_level_volume_budget() {
    for (Side side : {Side::BUY, Side::SELL}) {
        PairedBook books;
        TEST_ASSERT(books.insert(1, side, 1'000'000, MAX_VOLUME - 2).applied());
        TEST_ASSERT(books.insert(2, side, 1'000'000, 2).applied());
        books.rejected(books.insert(3, side, 1'000'000, 1),
            RejectReason::LEVEL_VOLUME_OVERFLOW);
        TEST_ASSERT(books.cancel(2).applied());
        TEST_ASSERT(books.insert(3, side, 1'000'000, 2).applied());
        const auto state = books.optimized.capture_state();
        const auto& level = same_side_level(state, side);
        TEST_ASSERT(level.total_volume == Quantity(MAX_VOLUME));
        TEST_ASSERT(level.orders.size() == 2);
        TEST_ASSERT(level.orders[0].order_id == OrderId(1));
        TEST_ASSERT(level.orders[1].order_id == OrderId(3));
    }
}

void test_level_volume_limit_with_finer_instrument_tick_and_full_pool() {
    InstrumentConfig rules;
    rules.tick_size = 1;
    for (Side side : {Side::BUY, Side::SELL}) {
        for (int64_t price : {int64_t(1'000'000), int64_t(100), int64_t(1'000'001)}) {
            // The last price is inside the ladder range but off its 100-unit
            // grid. It is valid with tick1 and must use the map fallback.
            PairedBook books(2, rules);
            TEST_ASSERT(books.insert(1, side, price, MAX_VOLUME).applied());
            // Move the same-side cache away from the target before checking
            // its volume: cover dense and map lookup, not just cache hits.
            TEST_ASSERT(books.insert(2, side, price + 100, 1).applied());
            auto before = books.optimized.capture_state();
            books.rejected(books.insert(3, side, price, 1),
                RejectReason::LEVEL_VOLUME_OVERFLOW);
            before.last_applied_command_sequence = CommandSequence(books.sequence);
            TEST_ASSERT(books.optimized.capture_state() == before);
        }
    }
}

void test_partial_trade_releases_level_volume_budget() {
    for (Side side : {Side::BUY, Side::SELL}) {
        PairedBook books;
        TEST_ASSERT(books.insert(1, side, 1'000'000, MAX_VOLUME).applied());
        TEST_ASSERT(books.insert(2, opposite(side), 1'000'000, 2, TimeInForce::IOC).applied());
        TEST_ASSERT(books.actual.events().size() == 1);
        TEST_ASSERT(std::get<TradeEvent>(books.actual.events()[0]).quantity == Quantity(2));
        TEST_ASSERT(books.insert(3, side, 1'000'000, 2).applied());
        const auto state = books.optimized.capture_state();
        const auto& level = same_side_level(state, side);
        TEST_ASSERT(level.total_volume == Quantity(MAX_VOLUME));
        TEST_ASSERT(level.orders[0].order_id == OrderId(1));
        TEST_ASSERT(level.orders[1].order_id == OrderId(3));
    }
}

void test_ioc_does_not_consume_level_volume_budget() {
    for (Side side : {Side::BUY, Side::SELL}) {
        PairedBook books;
        TEST_ASSERT(books.insert(1, side, 1'000'000, MAX_VOLUME).applied());
        auto before = books.optimized.capture_state();
        TEST_ASSERT(books.insert(2, side, 1'000'000, MAX_VOLUME, TimeInForce::IOC).applied());
        TEST_ASSERT(books.actual.events().size() == 1);
        TEST_ASSERT(std::get<OrderCancelledEvent>(books.actual.events()[0]).cancelled_quantity ==
            Quantity(MAX_VOLUME));
        before.last_applied_command_sequence = CommandSequence(books.sequence);
        TEST_ASSERT(books.optimized.capture_state() == before);
    }
}

void test_opposite_side_volume_is_not_a_resting_limit() {
    for (Side side : {Side::BUY, Side::SELL}) {
        PairedBook books;
        TEST_ASSERT(books.insert(1, opposite(side), 1'000'000, MAX_VOLUME).applied());
        TEST_ASSERT(books.insert(2, side, 1'000'000, MAX_VOLUME).applied());
        TEST_ASSERT(books.actual.events().size() == 1);
        TEST_ASSERT(std::get<TradeEvent>(books.actual.events()[0]).quantity == Quantity(MAX_VOLUME));
        TEST_ASSERT(books.optimized.active_order_count() == 0);
    }
}

void test_cross_level_trade_and_residual_near_volume_limit() {
    for (Side side : {Side::BUY, Side::SELL}) {
        PairedBook books;
        const int64_t best_price = side == Side::BUY ? 1'000'000 : 1'010'000;
        const int64_t limit_price = side == Side::BUY ? 1'010'000 : 1'000'000;
        TEST_ASSERT(books.insert(1, opposite(side), best_price, MAX_VOLUME - 2).applied());
        TEST_ASSERT(books.insert(2, opposite(side), limit_price, 1).applied());
        TEST_ASSERT(books.insert(3, side, limit_price, MAX_VOLUME).applied());
        TEST_ASSERT(books.actual.events().size() == 3);
        TEST_ASSERT(std::get<TradeEvent>(books.actual.events()[0]).quantity == Quantity(MAX_VOLUME - 2));
        TEST_ASSERT(std::get<TradeEvent>(books.actual.events()[1]).quantity == Quantity(1));
        const auto& rested = std::get<OrderRestedEvent>(books.actual.events()[2]);
        TEST_ASSERT(rested.remaining_quantity == Quantity(1));
        const auto state = books.optimized.capture_state();
        TEST_ASSERT(same_side_level(state, side).total_volume == Quantity(1));
    }
}

void test_post_only_rejection_precedes_pool_capacity() {
    for (Side side : {Side::BUY, Side::SELL}) {
        PairedBook books(1);
        TEST_ASSERT(books.insert(1, opposite(side), 1'000'000, 1).applied());
        auto before = books.optimized.capture_state();
        books.rejected(books.insert(2, side, 1'000'000, 1, TimeInForce::POST_ONLY),
            RejectReason::POST_ONLY_WOULD_TRADE);
        before.last_applied_command_sequence = CommandSequence(books.sequence);
        TEST_ASSERT(books.optimized.capture_state() == before);

        const int64_t passive_price = side == Side::BUY ? 990'000 : 1'010'000;
        books.rejected(books.insert(2, side, passive_price, 1, TimeInForce::POST_ONLY),
            RejectReason::POOL_EXHAUSTED);
        books.rejected(books.insert(2, side, 0, 1, TimeInForce::POST_ONLY),
            RejectReason::INVALID_PRICE);
        books.rejected(books.insert(1, side, 0, 1, TimeInForce::POST_ONLY),
            RejectReason::DUPLICATE_ORDER_ID);
    }
}

void test_new_rejection_reasons_diagnostic_roundtrip() {
    const std::vector<EngineEvent> events{
        OrderRejectedEvent(CommandSequence(1), EventIndex(0), CommandType::NEW_ORDER,
            OrderId(1), RejectReason::LEVEL_VOLUME_OVERFLOW),
        OrderRejectedEvent(CommandSequence(2), EventIndex(0), CommandType::NEW_ORDER,
            OrderId(2), RejectReason::POST_ONLY_WOULD_TRADE),
        OrderRejectedEvent(CommandSequence(3), EventIndex(0), CommandType::NEW_ORDER,
            OrderId(3), RejectReason::INVALID_SIDE),
        OrderRejectedEvent(CommandSequence(4), EventIndex(0), CommandType::NEW_ORDER,
            OrderId(4), RejectReason::INVALID_TIME_IN_FORCE)};
    const auto path = std::filesystem::temp_directory_path() /
        "matching_engine_m1_reject_reasons.csv";
    ReplayEngine::save_log(events, path.string());
    const auto restored = ReplayEngine::load_log(path.string());
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    TEST_ASSERT(restored == events);
}

void test_all_invalid_enum_bytes_reject_without_book_mutation() {
    PairedBook books(8);
    TEST_ASSERT(books.insert(1, Side::BUY, 990'000, 2).applied());
    TEST_ASSERT(books.insert(2, Side::SELL, 1'010'000, 2).applied());
    auto check = [&](Side side, TimeInForce tif, RejectReason reason) {
        auto before = books.optimized.capture_state();
        const auto capacity = books.optimized.capacity_metrics();
        books.rejected(books.insert(3, side, 1'000'000, 1, tif), reason);
        before.last_applied_command_sequence = CommandSequence(books.sequence);
        TEST_ASSERT(books.optimized.capture_state() == before);
        const auto after = books.optimized.capacity_metrics();
        TEST_ASSERT(after.active_orders == capacity.active_orders);
        TEST_ASSERT(after.active_levels == capacity.active_levels);
        TEST_ASSERT(after.index_load == capacity.index_load);
        TEST_ASSERT(after.peak_active_orders == capacity.peak_active_orders);
        TEST_ASSERT(after.peak_active_levels == capacity.peak_active_levels);
        const auto& event = std::get<OrderRejectedEvent>(books.actual.events().front());
        TEST_ASSERT(event.command_sequence == CommandSequence(books.sequence));
        TEST_ASSERT(event.event_index == EventIndex(0) && event.order_id == OrderId(3));
        TEST_ASSERT(books.optimized.ready_for_commands());
    };
    // Both enums use uint8_t. Exhaust every invalid representation, not just a
    // few cast values that happen to exercise the current implementation.
    for (unsigned byte = 2; byte <= 255; ++byte) {
        TEST_ASSERT(std::string(to_string(static_cast<Side>(byte))) == "UNKNOWN");
        check(static_cast<Side>(byte), TimeInForce::GTC, RejectReason::INVALID_SIDE);
    }
    for (unsigned byte = 3; byte <= 255; ++byte) {
        for (Side side : {Side::BUY, Side::SELL}) {
            check(side, static_cast<TimeInForce>(byte), RejectReason::INVALID_TIME_IN_FORCE);
        }
    }
    // Rejected IDs and order slots remain reusable; FIFO and index still agree.
    TEST_ASSERT(books.insert(3, Side::BUY, 1'000'000, 1).applied());
}

void test_enum_validation_rejection_priority() {
    PairedBook books(1);
    TEST_ASSERT(books.insert(1, Side::BUY, 1'000'000, 1).applied());
    const Side invalid_side = static_cast<Side>(255);
    const TimeInForce invalid_tif = static_cast<TimeInForce>(255);
    books.rejected(books.insert(1, invalid_side, 0, 0, invalid_tif), RejectReason::DUPLICATE_ORDER_ID);
    books.rejected(books.insert(0, invalid_side, 0, 0, invalid_tif), RejectReason::DUPLICATE_ORDER_ID);
    books.rejected(books.insert(2, invalid_side, 0, 0, invalid_tif), RejectReason::INVALID_SIDE);
    books.rejected(books.insert(2, Side::SELL, 0, 0, invalid_tif), RejectReason::INVALID_TIME_IN_FORCE);
    books.rejected(books.insert(2, Side::SELL, 0, 0), RejectReason::INVALID_PRICE);
    books.rejected(books.insert(2, Side::BUY, 990'000, 1), RejectReason::POOL_EXHAUSTED);

    const auto before = books.optimized.capture_state();
    const Command zero = NewOrderCommand(CommandSequence(0), OrderId(2), invalid_side,
        Price(0), Quantity(0), invalid_tif);
    TEST_ASSERT(books.process(zero).status == ProcessStatus::SEQUENCE_REJECTED);
    const Command repeated = NewOrderCommand(CommandSequence(books.sequence), OrderId(2),
        invalid_side, Price(0), Quantity(0), invalid_tif);
    TEST_ASSERT(books.process(repeated).status == ProcessStatus::SEQUENCE_REJECTED);
    TEST_ASSERT(books.actual.events().empty());
    TEST_ASSERT(books.optimized.capture_state() == before);
}

void test_enum_rejection_in_legacy_and_no_record_paths() {
    for (bool record : {true, false}) {
        OrderBook book(2, record);
        const auto result = book.process(NewOrderCommand(CommandSequence(1), OrderId(1),
            static_cast<Side>(2), Price(1'000'000), Quantity(1)));
        TEST_ASSERT(result.status == ProcessStatus::REJECTED);
        TEST_ASSERT(result.event_count == (record ? size_t{1} : size_t{0}));
        TEST_ASSERT(book.active_order_count() == 0 && book.check_invariants());
        TEST_ASSERT(book.last_applied_command_sequence() == CommandSequence(1));
        if (record) TEST_ASSERT(std::get<OrderRejectedEvent>(book.engine_events().back()).reason ==
            RejectReason::INVALID_SIDE);
        const auto tif = book.process(NewOrderCommand(CommandSequence(2), OrderId(1),
            Side::BUY, Price(1'000'000), Quantity(1), static_cast<TimeInForce>(3)));
        TEST_ASSERT(tif.status == ProcessStatus::REJECTED);
        TEST_ASSERT(book.active_order_count() == 0 && book.check_invariants());
        book.process_new_order(OrderId(1), static_cast<Side>(255), Price(1'000'000), Quantity(1));
        TEST_ASSERT(book.active_order_count() == 0);
        TEST_ASSERT(book.last_applied_command_sequence() == CommandSequence(3));
    }
}

void test_output_barrier_precedes_enum_validation() {
    OrderBook book(2);
    FixedEventBuffer<0> full;
    TEST_ASSERT(book.process(NewOrderCommand(CommandSequence(1), OrderId(1), Side::BUY,
        Price(1'000'000), Quantity(1)), full).applied());
    const Command invalid = NewOrderCommand(CommandSequence(2), OrderId(2),
        static_cast<Side>(255), Price(1'000'000), Quantity(1));
    VectorEventSink received;
    TEST_ASSERT(book.process(invalid, received).status == ProcessStatus::OUTPUT_BLOCKED);
    TEST_ASSERT(book.last_applied_command_sequence() == CommandSequence(1));
    TEST_ASSERT(book.pending_event_count() == 1 && received.events().empty());
    TEST_ASSERT(book.retry_pending_events(received));
    received.clear();
    TEST_ASSERT(book.process(invalid, received).status == ProcessStatus::REJECTED);
    TEST_ASSERT(std::get<OrderRejectedEvent>(received.events().front()).reason == RejectReason::INVALID_SIDE);
    TEST_ASSERT(book.active_order_count() == 1 && book.check_invariants());
}

void test_enum_rejection_event_can_be_resumed_without_reexecution() {
    OrderBook book(2);
    FixedEventBuffer<0> full;
    const auto rejected = book.process(NewOrderCommand(CommandSequence(1), OrderId(1),
        Side::BUY, Price(1'000'000), Quantity(1), static_cast<TimeInForce>(255)), full);
    TEST_ASSERT(rejected.status == ProcessStatus::REJECTED && !rejected.event_output_complete);
    TEST_ASSERT(book.active_order_count() == 0 && book.pending_event_count() == 1);
    const Command valid = NewOrderCommand(CommandSequence(2), OrderId(1), Side::BUY,
        Price(1'000'000), Quantity(1));
    VectorEventSink received;
    TEST_ASSERT(book.process(valid, received).status == ProcessStatus::OUTPUT_BLOCKED);
    TEST_ASSERT(book.last_applied_command_sequence() == CommandSequence(1));
    TEST_ASSERT(book.retry_pending_events(received));
    const auto& event = std::get<OrderRejectedEvent>(received.events().at(0));
    TEST_ASSERT(event.reason == RejectReason::INVALID_TIME_IN_FORCE);
    TEST_ASSERT(event.command_sequence == CommandSequence(1) && event.event_index == EventIndex(0));
    received.clear();
    TEST_ASSERT(book.process(valid, received).applied());
    TEST_ASSERT(book.active_order_count() == 1 && book.check_invariants());
}

} // namespace

int main() {
    try {
        test_all_invalid_enum_bytes_reject_without_book_mutation();
        test_enum_validation_rejection_priority();
        test_enum_rejection_in_legacy_and_no_record_paths();
        test_output_barrier_precedes_enum_validation();
        test_enum_rejection_event_can_be_resumed_without_reexecution();
        test_level_volume_limit_and_atomic_rejection();
        test_cancel_releases_level_volume_budget();
        test_level_volume_limit_with_finer_instrument_tick_and_full_pool();
        test_partial_trade_releases_level_volume_budget();
        test_ioc_does_not_consume_level_volume_budget();
        test_opposite_side_volume_is_not_a_resting_limit();
        test_cross_level_trade_and_residual_near_volume_limit();
        test_post_only_rejection_precedes_pool_capacity();
        test_new_rejection_reasons_diagnostic_roundtrip();
        OrderBook book(16, true);
        book.process(NewOrderCommand(CommandSequence(1), OrderId(1), Side::SELL,
                                     Price(1000000), Quantity(3)));
        book.process(NewOrderCommand(CommandSequence(2), OrderId(2), Side::SELL,
                                     Price(1010000), Quantity(5)));

        const ProcessResult post_only = book.process(NewOrderCommand(
            CommandSequence(3), OrderId(3), Side::BUY, Price(1000000), Quantity(1),
            TimeInForce::POST_ONLY));
        TEST_ASSERT(post_only.status == ProcessStatus::REJECTED);
        TEST_ASSERT(book.active_order_count() == 2);
        const auto& rejected = std::get<OrderRejectedEvent>(book.engine_events().back());
        TEST_ASSERT(rejected.reason == RejectReason::POST_ONLY_WOULD_TRADE);

        const ProcessResult resting_post_only = book.process(NewOrderCommand(
            CommandSequence(4), OrderId(4), Side::BUY, Price(990000), Quantity(2),
            TimeInForce::POST_ONLY));
        TEST_ASSERT(resting_post_only.status == ProcessStatus::APPLIED);
        TEST_ASSERT(book.active_order_count() == 3);

        const size_t begin = book.engine_events().size();
        const ProcessResult ioc = book.process(NewOrderCommand(
            CommandSequence(5), OrderId(5), Side::BUY, Price(1010000), Quantity(10),
            TimeInForce::IOC));
        TEST_ASSERT(ioc.status == ProcessStatus::APPLIED);
        TEST_ASSERT(ioc.event_count == 3); // Two trades, then residual cancel.
        TEST_ASSERT(std::holds_alternative<OrderCancelledEvent>(
            book.engine_events()[begin + 2]));
        const auto& cancelled = std::get<OrderCancelledEvent>(book.engine_events()[begin + 2]);
        TEST_ASSERT(cancelled.cancelled_quantity == Quantity(2));
        TEST_ASSERT(book.active_order_count() == 1); // Only resting post-only bid remains.
        TEST_ASSERT(book.check_invariants());

        const SystemEvent event = SystemEvent::order(Timestamp(10), Symbol("AAPL"),
            NewOrderCommand(CommandSequence(6), OrderId(6), Side::BUY,
                            Price(900000), Quantity(1), TimeInForce::IOC));
        const auto record = JournalProjector::project(event, 1);
        TEST_ASSERT(record && record->time_in_force == TimeInForce::IOC);

        const std::string journal_path = "order_type_replay_test.csv";
        MarketManager source(16);
        EventDispatcher source_dispatcher;
        Journal journal;
        journal.attach(source_dispatcher);
        TradingEngine source_engine(source, source_dispatcher);
        source_engine.submit(Timestamp(1), Symbol("AAPL"),
            NewOrderCommand(CommandSequence(1), OrderId(100), Side::SELL,
                            Price(1000000), Quantity(5)));
        source_engine.submit(Timestamp(2), Symbol("AAPL"),
            NewOrderCommand(CommandSequence(2), OrderId(101), Side::BUY,
                            Price(1000000), Quantity(7), TimeInForce::IOC));
        journal.save(journal_path);
        const auto loaded = Journal::load(journal_path);
        bool loaded_ioc = false;
        for (const JournalRecord& item : loaded) {
            if (item.type == JournalRecordType::ORDER_INSERT &&
                item.order_id == OrderId(101)) {
                loaded_ioc = item.time_in_force == TimeInForce::IOC;
            }
        }
        TEST_ASSERT(loaded_ioc);
        MarketManager replayed(16);
        EventDispatcher replay_dispatcher;
        TradingEngine replay_engine(replayed, replay_dispatcher);
        const JournalReplayResult replay_result = JournalReplayEngine::replay(
            loaded, replay_engine, replay_dispatcher, &source);
        TEST_ASSERT(replay_result.success());
        std::error_code ignored;
        std::filesystem::remove(journal_path, ignored);
        std::cout << "Order type tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Order type tests failed: " << error.what() << '\n';
        return 1;
    }
}
