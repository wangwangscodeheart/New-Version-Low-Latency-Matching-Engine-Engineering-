#include "event_sink.hpp"
#include "market_snapshot.hpp"
#include "trading_engine.hpp"
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>

#define TEST_ASSERT(x) do { if (!(x)) throw std::runtime_error("assertion failed: " #x); } while(false)

namespace {

Command new_order(uint64_t sequence, Side side = Side::BUY) {
    return NewOrderCommand(CommandSequence(sequence), OrderId(sequence), side,
        Price(1'000'000), Quantity(1));
}

void test_blocked_book_is_rejected_before_order_publication() {
    MarketManager markets(4);
    FixedEventBuffer<0> full;
    const auto first = markets.process(Symbol("AAPL"), new_order(1), full);
    TEST_ASSERT(first.process_result.applied());
    TEST_ASSERT(!first.process_result.event_output_complete);
    const auto state = markets.find_book(Symbol("AAPL"))->capture_state();
    EventDispatcher dispatcher;
    size_t calls = 0;
    auto subscription = dispatcher.subscribe_all([&](const SystemEvent&) { ++calls; });
    dispatcher.freeze();
    TradingEngine engine(markets, dispatcher);
    const auto blocked = engine.submit(Timestamp(2), Symbol("AAPL"), new_order(2));
    TEST_ASSERT(blocked.routing_status == RoutingStatus::SYSTEM_UNAVAILABLE);
    TEST_ASSERT(blocked.process_result.status == ProcessStatus::OUTPUT_BLOCKED);
    TEST_ASSERT(calls == 0);
    TEST_ASSERT(markets.find_book(Symbol("AAPL"))->capture_state() == state);
    // A direct per-book sink backlog does not silently block unrelated books.
    TEST_ASSERT(engine.submit(Timestamp(2), Symbol("TSLA"), new_order(1)).process_result.applied());
    TEST_ASSERT(!engine.failed());
    VectorEventSink resumed;
    TEST_ASSERT(markets.find_book(Symbol("AAPL"))->retry_pending_events(resumed));
    TEST_ASSERT(resumed.events().size() == 1);
    TEST_ASSERT(engine.submit(Timestamp(3), Symbol("AAPL"), new_order(2)).process_result.applied());
}

void test_engine_reentrancy_never_publishes_nested_commands() {
    for (SystemEventType trigger : {SystemEventType::ORDER, SystemEventType::ORDER_RESTED,
                                   SystemEventType::PROCESSING_LATENCY}) {
        MarketManager markets(4);
        EventDispatcher dispatcher;
        TradingEngine engine(markets, dispatcher);
        std::optional<MarketProcessResult> nested;
        size_t orders = 0;
        auto subscription = dispatcher.subscribe_all([&](const SystemEvent& event) {
            if (event.event_type == SystemEventType::ORDER) ++orders;
            if (event.event_type == trigger && !nested) {
                // Set a value before calling in case the implementation recurses.
                nested = MarketProcessResult{RoutingStatus::UNKNOWN_SYMBOL,
                    ProcessResult{ProcessStatus::REJECTED, 0, 0}};
                nested = engine.submit(Timestamp(10), Symbol("TSLA"), new_order(1));
            }
        });
        dispatcher.freeze();
        TEST_ASSERT(engine.submit(Timestamp(1), Symbol("AAPL"), new_order(1)).process_result.applied());
        TEST_ASSERT(nested && nested->routing_status == RoutingStatus::SYSTEM_UNAVAILABLE);
        TEST_ASSERT(nested->process_result.status == ProcessStatus::OUTPUT_BLOCKED);
        TEST_ASSERT(orders == 1);
        TEST_ASSERT(markets.find_book(Symbol("TSLA"))->active_order_count() == 0);
        TEST_ASSERT(!engine.failed());
    }
}

void test_dispatcher_exceptions_fail_engine_closed() {
    for (SystemEventType trigger : {SystemEventType::ORDER, SystemEventType::ORDER_RESTED,
                                   SystemEventType::PROCESSING_LATENCY}) {
        MarketManager markets(4);
        EventDispatcher dispatcher;
        TradingEngine engine(markets, dispatcher);
        size_t before_throw = 0;
        size_t after_throw = 0;
        auto first = dispatcher.subscribe_all([&](const SystemEvent& event) {
            if (event.event_type == trigger) ++before_throw;
        });
        auto failing = dispatcher.subscribe_all([&](const SystemEvent& event) {
            if (event.event_type == trigger) throw std::runtime_error("injected callback failure");
        });
        auto last = dispatcher.subscribe_all([&](const SystemEvent& event) {
            if (event.event_type == trigger) ++after_throw;
        });
        dispatcher.freeze();
        bool threw = false;
        try { engine.submit(Timestamp(1), Symbol("AAPL"), new_order(1)); }
        catch (const std::runtime_error&) { threw = true; }
        TEST_ASSERT(threw && engine.failed());
        TEST_ASSERT(before_throw == 1 && after_throw == 0);
        const auto state = markets.find_book(Symbol("AAPL"))->capture_state();
        const auto next = engine.submit(Timestamp(2), Symbol("TSLA"), new_order(1));
        TEST_ASSERT(next.routing_status == RoutingStatus::SYSTEM_UNAVAILABLE);
        TEST_ASSERT(before_throw == 1 && after_throw == 0);
        TEST_ASSERT(markets.find_book(Symbol("AAPL"))->capture_state() == state);
        TEST_ASSERT(markets.find_book(Symbol("TSLA"))->active_order_count() == 0);
        TEST_ASSERT(state.active_order_count == (trigger == SystemEventType::ORDER ? 0u : 1u));
        if (trigger == SystemEventType::ORDER_RESTED) {
            const auto* book = markets.find_book(Symbol("AAPL"));
            TEST_ASSERT(book->event_output_state() == EventOutputState::FAILED);
            TEST_ASSERT(book->pending_event_count() == 1);
        }
    }
}

void test_market_data_exception_stops_future_commands() {
    MarketManager markets(4);
    EventDispatcher dispatcher;
    TradingEngine engine(markets, dispatcher);
    size_t calls = 0;
    auto subscription = dispatcher.subscribe(SystemEventType::MARKET_DATA,
        [&](const SystemEvent&) { ++calls; throw std::runtime_error("injected quote callback failure"); });
    dispatcher.freeze();
    bool threw = false;
    try {
        engine.publish_market_data(Timestamp(1), Symbol("AAPL"),
            MarketDataEvent{Price(100), Quantity(1), Price(100), Price(200)});
    } catch (const std::runtime_error&) { threw = true; }
    TEST_ASSERT(threw && engine.failed() && calls == 1);
    TEST_ASSERT(engine.submit(Timestamp(2), Symbol("AAPL"), new_order(1)).routing_status ==
        RoutingStatus::SYSTEM_UNAVAILABLE);
    TEST_ASSERT(markets.find_book(Symbol("AAPL"))->active_order_count() == 0);
    threw = false;
    try {
        engine.publish_market_data(Timestamp(3), Symbol("AAPL"),
            MarketDataEvent{Price(100), Quantity(1), Price(100), Price(200)});
    } catch (const std::logic_error&) { threw = true; }
    TEST_ASSERT(threw && calls == 1);
}

void test_snapshot_preflight_leaves_no_generation_files() {
    const auto base = std::filesystem::temp_directory_path();
    std::filesystem::path directory;
    for (unsigned i = 0; i < 1024; ++i) {
        const auto candidate = base / ("matching_engine_output_barrier_" + std::to_string(i));
        if (std::filesystem::create_directory(candidate)) { directory = candidate; break; }
    }
    TEST_ASSERT(!directory.empty());
    struct Cleanup {
        std::filesystem::path directory;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(directory, ignored); }
    } cleanup{directory};
    MarketManager markets(4);
    FixedEventBuffer<0> full;
    // TSLA sorts after AAPL/NVDA. Without all-books preflight those earlier
    // generation files would already be written when TSLA capture fails.
    TEST_ASSERT(markets.process(Symbol("TSLA"), new_order(1), full).process_result.applied());
    bool rejected = false;
    try { MarketSnapshotStore::save_atomic(directory / "market.snapshot", 7, markets); }
    catch (const std::logic_error&) { rejected = true; }
    TEST_ASSERT(rejected && std::filesystem::is_empty(directory));
    VectorEventSink resumed;
    TEST_ASSERT(markets.find_book(Symbol("TSLA"))->retry_pending_events(resumed));
    MarketSnapshotStore::save_atomic(directory / "market.snapshot", 7, markets);
    const auto restored = MarketSnapshotStore::load(directory / "market.snapshot");
    TEST_ASSERT(restored.success());
    TEST_ASSERT(restored.markets->find_book(Symbol("TSLA"))->capture_state() ==
        markets.find_book(Symbol("TSLA"))->capture_state());
}

void test_capacity_index_limit_is_checked_before_allocation() {
    if (std::numeric_limits<size_t>::max() > std::numeric_limits<uint32_t>::max()) {
        bool rejected = false;
        try { OrderBook too_large(static_cast<size_t>(std::numeric_limits<uint32_t>::max()) + 1, false); }
        catch (const std::length_error&) { rejected = true; }
        TEST_ASSERT(rejected);
    }
}

} // namespace

int main() {
    try {
        test_blocked_book_is_rejected_before_order_publication();
        test_engine_reentrancy_never_publishes_nested_commands();
        test_dispatcher_exceptions_fail_engine_closed();
        test_market_data_exception_stops_future_commands();
        test_snapshot_preflight_leaves_no_generation_files();
        test_capacity_index_limit_is_checked_before_allocation();
        std::cout << "Event delivery integration tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Event delivery integration tests failed: " << error.what() << '\n';
        return 1;
    }
}
