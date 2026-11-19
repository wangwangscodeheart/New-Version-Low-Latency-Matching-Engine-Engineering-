#include "trading_engine.hpp"
#include "reference_engine.hpp"
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

#define TEST_ASSERT(condition)                                                \
    do {                                                                      \
        if (!(condition)) {                                                   \
            throw std::runtime_error("Assertion failed: " #condition);       \
        }                                                                     \
    } while (false)

void test_dispatch_order_and_filtering() {
    EventDispatcher dispatcher;
    std::vector<int> calls;
    auto all = dispatcher.subscribe_all([&](const SystemEvent&) { calls.push_back(1); });
    auto first = dispatcher.subscribe(SystemEventType::MARKET_DATA,
        [&](const SystemEvent&) { calls.push_back(2); });
    auto second = dispatcher.subscribe(SystemEventType::MARKET_DATA,
        [&](const SystemEvent&) { calls.push_back(3); });
    (void)all; (void)first; (void)second;

    dispatcher.publish(SystemEvent::market_data(
        Timestamp(10), Symbol("AAPL"),
        MarketDataEvent{from_double(100), Quantity(20),
                        from_double(99.99), from_double(100.01)}));
    TEST_ASSERT((calls == std::vector<int>{1, 2, 3}));
}

void test_order_to_trade_event_flow() {
    MarketManager markets(64);
    EventDispatcher dispatcher;
    TradingEngine engine(markets, dispatcher);
    std::vector<SystemEventType> types;
    std::vector<std::string> symbols;
    auto subscription = dispatcher.subscribe_all([&](const SystemEvent& event) {
        types.push_back(event.event_type);
        symbols.push_back(event.symbol.value());
    });
    (void)subscription;

    engine.submit(Timestamp(100), Symbol("AAPL"),
        NewOrderCommand(CommandSequence(1), OrderId(1), Side::SELL,
                        from_double(100), Quantity(10)));
    engine.submit(Timestamp(101), Symbol("AAPL"),
        NewOrderCommand(CommandSequence(2), OrderId(2), Side::BUY,
                        from_double(100), Quantity(10)));

    TEST_ASSERT((types == std::vector<SystemEventType>{
        SystemEventType::ORDER, SystemEventType::ORDER_RESTED,
        SystemEventType::PROCESSING_LATENCY, SystemEventType::ORDER,
        SystemEventType::TRADE, SystemEventType::PROCESSING_LATENCY}));
    TEST_ASSERT(symbols.size() == 6);
    for (const std::string& symbol : symbols) TEST_ASSERT(symbol == "AAPL");
}

void test_cancel_and_reject_mapping() {
    MarketManager markets(64);
    EventDispatcher dispatcher;
    TradingEngine engine(markets, dispatcher);
    std::vector<SystemEventType> engine_types;
    auto subscription = dispatcher.subscribe_all([&](const SystemEvent& event) {
        if (event.event_type != SystemEventType::ORDER &&
            event.event_type != SystemEventType::PROCESSING_LATENCY) {
            engine_types.push_back(event.event_type);
        }
    });
    (void)subscription;

    engine.submit(Timestamp(1), Symbol("TSLA"),
        NewOrderCommand(CommandSequence(1), OrderId(7), Side::BUY,
                        from_double(200), Quantity(5)));
    engine.submit(Timestamp(2), Symbol("TSLA"),
        CancelOrderCommand(CommandSequence(2), OrderId(7)));
    engine.submit(Timestamp(3), Symbol("TSLA"),
        CancelOrderCommand(CommandSequence(3), OrderId(999)));

    TEST_ASSERT((engine_types == std::vector<SystemEventType>{
        SystemEventType::ORDER_RESTED, SystemEventType::CANCEL,
        SystemEventType::ORDER_REJECTED}));
}

void test_market_data_does_not_mutate_book() {
    MarketManager markets(64);
    EventDispatcher dispatcher;
    TradingEngine engine(markets, dispatcher);
    size_t market_events = 0;
    auto subscription = dispatcher.subscribe(SystemEventType::MARKET_DATA,
        [&](const SystemEvent& event) {
            ++market_events;
            TEST_ASSERT(std::holds_alternative<MarketDataEvent>(event.payload));
        });
    (void)subscription;

    const uint64_t before = markets.find_book(Symbol("NVDA"))->state_hash();
    engine.publish_market_data(Timestamp(5), Symbol("NVDA"),
        MarketDataEvent{from_double(150), Quantity(100),
                        from_double(149.99), from_double(150.01)});
    TEST_ASSERT(market_events == 1);
    TEST_ASSERT(markets.find_book(Symbol("NVDA"))->state_hash() == before);
}

void test_subscription_lifetime() {
    EventDispatcher dispatcher;
    size_t calls = 0;
    {
        auto subscription = dispatcher.subscribe(SystemEventType::MARKET_DATA,
            [&](const SystemEvent&) { ++calls; });
        TEST_ASSERT(subscription.active());
        TEST_ASSERT(dispatcher.subscriber_count(SystemEventType::MARKET_DATA) == 1);
    }
    TEST_ASSERT(dispatcher.subscriber_count(SystemEventType::MARKET_DATA) == 0);
    dispatcher.publish(SystemEvent::market_data(Timestamp(1), Symbol("AAPL"),
        MarketDataEvent{Price(1), Quantity(1), Price(1), Price(1)}));
    TEST_ASSERT(calls == 0);

    EventDispatcher::Subscription surviving_token;
    {
        EventDispatcher temporary;
        surviving_token = temporary.subscribe_all([](const SystemEvent&) {});
        TEST_ASSERT(surviving_token.active());
    }
    TEST_ASSERT(!surviving_token.active());
    surviving_token.reset();
}

void test_unsubscribe_during_dispatch_affects_next_event() {
    EventDispatcher dispatcher;
    std::vector<int> calls;
    EventDispatcher::Subscription second;
    auto first = dispatcher.subscribe(SystemEventType::MARKET_DATA,
        [&](const SystemEvent&) {
            calls.push_back(1);
            second.reset();
        });
    second = dispatcher.subscribe(SystemEventType::MARKET_DATA,
        [&](const SystemEvent&) { calls.push_back(2); });
    (void)first;
    const SystemEvent event = SystemEvent::market_data(Timestamp(1), Symbol("AAPL"),
        MarketDataEvent{Price(1), Quantity(1), Price(1), Price(1)});
    dispatcher.publish(event);
    dispatcher.publish(event);
    TEST_ASSERT((calls == std::vector<int>{1, 2, 1}));
}

void test_v2_sink_drains_order_book_history() {
    MarketManager markets(16);
    EventDispatcher dispatcher;
    TradingEngine engine(markets, dispatcher);
    std::vector<EngineEvent> received;
    auto subscription = dispatcher.subscribe(SystemEventType::ORDER_RESTED,
        [&](const SystemEvent& event) {
            received.push_back(std::get<EngineEvent>(event.payload));
        });
    (void)subscription;

    const MarketProcessResult result = engine.submit(Timestamp(1), Symbol("AAPL"),
        NewOrderCommand(CommandSequence(1), OrderId(1), Side::BUY,
                        from_double(100), Quantity(1)));
    TEST_ASSERT(result.routed());
    TEST_ASSERT(result.process_result.event_output_complete);
    TEST_ASSERT(received.size() == 1);
    TEST_ASSERT(markets.find_book(Symbol("AAPL"))->engine_events().empty());
}

void test_fixed_sink_reports_overflow() {
    OrderBook book(16);
    book.process(NewOrderCommand(CommandSequence(1), OrderId(1), Side::SELL,
                                 from_double(100), Quantity(1)));
    book.process(NewOrderCommand(CommandSequence(2), OrderId(2), Side::SELL,
                                 from_double(101), Quantity(1)));
    FixedEventBuffer<1> sink;
    const ProcessResult result = book.process(
        NewOrderCommand(CommandSequence(3), OrderId(3), Side::BUY,
                        from_double(101), Quantity(2)), sink);
    TEST_ASSERT(result.applied());
    TEST_ASSERT(result.event_count == 2);
    TEST_ASSERT(!result.event_output_complete);
    TEST_ASSERT(sink.size() == 1);
    TEST_ASSERT(book.engine_events().size() == 2);
    TEST_ASSERT(book.pending_event_count() == 1);
    TEST_ASSERT(book.event_output_state() == EventOutputState::PENDING);
    TEST_ASSERT(!book.ready_for_commands());
    TEST_ASSERT(book.active_order_count() == 0);
    TEST_ASSERT(book.check_invariants());
    const auto retained = book.engine_events();
    TEST_ASSERT(sink[0] == retained[0]);
    VectorEventSink suffix;
    TEST_ASSERT(book.retry_pending_events(suffix));
    TEST_ASSERT(suffix.events().size() == 1);
    TEST_ASSERT(suffix.events()[0] == retained[1]);
    TEST_ASSERT(book.engine_events().empty());
    TEST_ASSERT(book.pending_event_count() == 0);
    TEST_ASSERT(book.event_output_state() == EventOutputState::READY);
    TEST_ASSERT(book.ready_for_commands());
}

namespace {

struct BudgetSink {
    size_t limit;
    size_t calls = 0;
    std::vector<EngineEvent> accepted;

    explicit BudgetSink(size_t capacity) : limit(capacity) {}

    bool push(const EngineEvent& event) {
        ++calls;
        if (accepted.size() == limit) return false;
        accepted.push_back(event);
        return true;
    }
};

struct CountingSink {
    size_t calls = 0;
    bool push(const EngineEvent&) {
        ++calls;
        return true;
    }
};

struct ThrowingSink {
    bool accept_first;
    size_t calls = 0;
    std::vector<EngineEvent> accepted;

    explicit ThrowingSink(bool accept) : accept_first(accept) {}

    bool push(const EngineEvent& event) {
        ++calls;
        if (accept_first) accepted.push_back(event);
        throw std::runtime_error("injected sink failure");
    }
};

void assert_snapshot_blocked(const OrderBook& book) {
    bool blocked = false;
    try {
        (void)book.create_snapshot();
    } catch (const std::logic_error&) {
        blocked = true;
    }
    TEST_ASSERT(blocked);
}

void assert_command_blocked(const ProcessResult& result) {
    TEST_ASSERT(result.status == ProcessStatus::OUTPUT_BLOCKED);
    TEST_ASSERT(result.event_count == 0);
    TEST_ASSERT(!result.event_output_complete);
}

void seed_three_asks(OrderBook& book, ReferenceEngine& reference) {
    NullEventSink sink;
    std::vector<EngineEvent> expected;
    for (uint64_t i = 0; i < 3; ++i) {
        const Command command = NewOrderCommand(CommandSequence(i + 1), OrderId(i + 1),
            Side::SELL, from_double(100 + static_cast<double>(i)), Quantity(1));
        TEST_ASSERT(book.process(command, sink).applied());
        TEST_ASSERT(reference.process(command, expected) == ProcessStatus::APPLIED);
        TEST_ASSERT(book.ready_for_commands());
    }
    TEST_ASSERT(book.capture_state() == reference.capture_state());
}

Command sweep_command(TimeInForce tif = TimeInForce::GTC) {
    return NewOrderCommand(CommandSequence(4), OrderId(4), Side::BUY,
        from_double(102), Quantity(4), tif);
}

void test_pending_suffix_retries_are_ordered_and_block_both_inputs() {
    OrderBook book(4, true, {}, 1);
    ReferenceEngine reference(4);
    seed_three_asks(book, reference);
    std::vector<EngineEvent> expected;
    TEST_ASSERT(reference.process(sweep_command(), expected) == ProcessStatus::APPLIED);
    BudgetSink prefix{1};
    const auto applied = book.process(sweep_command(), prefix);
    TEST_ASSERT(applied.applied() && applied.event_count == 4);
    TEST_ASSERT(!applied.event_output_complete);
    TEST_ASSERT(prefix.accepted.size() == 1);
    TEST_ASSERT(book.pending_event_count() == 3);
    TEST_ASSERT(book.engine_events() == expected);
    TEST_ASSERT(book.capture_state() == reference.capture_state());
    assert_snapshot_blocked(book);

    const Command next = NewOrderCommand(CommandSequence(5), OrderId(5), Side::SELL,
        from_double(105), Quantity(1));
    const auto state_before = book.capture_state();
    const size_t capacity_before = book.event_log_capacity();
    CountingSink unused;
    assert_command_blocked(book.process(next));
    assert_command_blocked(book.process(next, unused));
    TEST_ASSERT(unused.calls == 0);
    TEST_ASSERT(book.capture_state() == state_before);
    TEST_ASSERT(book.engine_events() == expected);
    TEST_ASSERT(book.last_applied_command_sequence() == CommandSequence(4));

    // A repeated refusal accepts nothing and must not consume the cursor.
    TEST_ASSERT(!book.retry_pending_events(prefix));
    TEST_ASSERT(prefix.accepted.size() == 1);
    TEST_ASSERT(book.pending_event_count() == 3);
    prefix.limit = 2;
    TEST_ASSERT(!book.retry_pending_events(prefix));
    TEST_ASSERT(prefix.accepted.size() == 2);
    TEST_ASSERT(book.pending_event_count() == 2);
    TEST_ASSERT(book.engine_events() == expected);

    VectorEventSink suffix;
    TEST_ASSERT(book.retry_pending_events(suffix));
    std::vector<EngineEvent> delivered = prefix.accepted;
    delivered.insert(delivered.end(), suffix.events().begin(), suffix.events().end());
    TEST_ASSERT(delivered == expected); // Includes every sequence/index/payload field.
    TEST_ASSERT(book.event_output_state() == EventOutputState::READY);
    TEST_ASSERT(book.ready_for_commands());
    TEST_ASSERT(book.pending_event_count() == 0);
    TEST_ASSERT(book.engine_events().empty());
    TEST_ASSERT(book.event_log_capacity() == capacity_before);
    TEST_ASSERT(book.create_snapshot().last_applied_command_sequence == CommandSequence(4));

    // The same previously blocked sequence and ID remain admissible.
    suffix.clear();
    TEST_ASSERT(book.process(next, suffix).applied());
    TEST_ASSERT(reference.process(next, expected) == ProcessStatus::APPLIED);
    TEST_ASSERT(suffix.events() == expected);
    TEST_ASSERT(book.capture_state() == reference.capture_state());
    TEST_ASSERT(book.check_invariants());
}

void test_zero_capacity_sink_retains_the_entire_batch() {
    OrderBook book(4, true, {}, 1);
    ReferenceEngine reference(4);
    seed_three_asks(book, reference);
    std::vector<EngineEvent> expected;
    reference.process(sweep_command(), expected);
    FixedEventBuffer<0> none;
    const auto result = book.process(sweep_command(), none);
    TEST_ASSERT(result.applied() && !result.event_output_complete);
    TEST_ASSERT(none.size() == 0);
    TEST_ASSERT(book.pending_event_count() == 4);
    TEST_ASSERT(book.engine_events() == expected);
    TEST_ASSERT(!book.retry_pending_events(none));
    TEST_ASSERT(!book.retry_pending_events(none));
    TEST_ASSERT(book.pending_event_count() == 4);
    VectorEventSink recovered;
    TEST_ASSERT(book.retry_pending_events(recovered));
    TEST_ASSERT(recovered.events() == expected);
    TEST_ASSERT(book.ready_for_commands() && book.check_invariants());

    // Even a zero-order-capacity book needs room for one rejection event.
    OrderBook empty(0, true, {}, 0);
    TEST_ASSERT(empty.event_log_capacity() >= 1);
    const auto rejected = empty.process(NewOrderCommand(CommandSequence(1), OrderId(1),
        Side::BUY, from_double(100), Quantity(1)), none);
    TEST_ASSERT(rejected.status == ProcessStatus::REJECTED);
    TEST_ASSERT(rejected.event_count == 1 && !rejected.event_output_complete);
    TEST_ASSERT(empty.pending_event_count() == 1);
    VectorEventSink rejection;
    TEST_ASSERT(empty.retry_pending_events(rejection));
    TEST_ASSERT(rejection.events().size() == 1);
    TEST_ASSERT(std::get<OrderRejectedEvent>(rejection.events()[0]).reason ==
        RejectReason::POOL_EXHAUSTED);
    TEST_ASSERT(empty.ready_for_commands());
}

void test_move_preserves_pending_batch_and_delivery_cursor() {
    OrderBook original(4);
    ReferenceEngine reference(4);
    seed_three_asks(original, reference);
    std::vector<EngineEvent> expected;
    reference.process(sweep_command(), expected);
    BudgetSink prefix{1};
    TEST_ASSERT(!original.process(sweep_command(), prefix).event_output_complete);
    const auto state_before = original.capture_state();
    const size_t buffer_capacity = original.event_log_capacity();
    OrderBook moved(std::move(original));
    TEST_ASSERT(moved.event_output_state() == EventOutputState::PENDING);
    TEST_ASSERT(!moved.ready_for_commands());
    TEST_ASSERT(moved.pending_event_count() == 3);
    TEST_ASSERT(moved.engine_events() == expected);
    TEST_ASSERT(moved.capture_state() == state_before);
    TEST_ASSERT(moved.event_log_capacity() == buffer_capacity);
    assert_snapshot_blocked(moved);
    VectorEventSink suffix;
    TEST_ASSERT(moved.retry_pending_events(suffix));
    auto delivered = prefix.accepted;
    delivered.insert(delivered.end(), suffix.events().begin(), suffix.events().end());
    TEST_ASSERT(delivered == expected);
    TEST_ASSERT(moved.ready_for_commands() && moved.check_invariants());
}

void test_sink_exceptions_fail_closed_without_ambiguous_retries() {
    for (bool accept_first : {false, true}) {
        OrderBook book(4);
        ThrowingSink throwing{accept_first};
        bool propagated = false;
        try {
            (void)book.process(NewOrderCommand(CommandSequence(1), OrderId(1), Side::BUY,
                from_double(100), Quantity(1)), throwing);
        } catch (const std::runtime_error&) {
            propagated = true;
        }
        TEST_ASSERT(propagated);
        TEST_ASSERT(throwing.calls == 1);
        TEST_ASSERT(throwing.accepted.size() == (accept_first ? size_t(1) : size_t(0)));
        TEST_ASSERT(book.event_output_state() == EventOutputState::FAILED);
        TEST_ASSERT(!book.ready_for_commands());
        TEST_ASSERT(book.active_order_count() == 1);
        TEST_ASSERT(book.last_applied_command_sequence() == CommandSequence(1));
        TEST_ASSERT(book.check_invariants());
        assert_snapshot_blocked(book);
        const auto before = book.capture_state();
        CountingSink unused;
        TEST_ASSERT(!book.retry_pending_events(unused));
        TEST_ASSERT(!book.retry_pending_events(unused));
        const Command next = CancelOrderCommand(CommandSequence(2), OrderId(1));
        assert_command_blocked(book.process(next));
        assert_command_blocked(book.process(next, unused));
        TEST_ASSERT(unused.calls == 0);
        TEST_ASSERT(book.capture_state() == before);
    }
}

void test_exception_while_retrying_pending_batch_is_terminal() {
    OrderBook book(4);
    FixedEventBuffer<0> none;
    TEST_ASSERT(!book.process(NewOrderCommand(CommandSequence(1), OrderId(1), Side::BUY,
        from_double(100), Quantity(1)), none).event_output_complete);
    ThrowingSink throwing{true};
    bool propagated = false;
    try {
        (void)book.retry_pending_events(throwing);
    } catch (const std::runtime_error&) {
        propagated = true;
    }
    TEST_ASSERT(propagated && throwing.accepted.size() == 1);
    TEST_ASSERT(book.event_output_state() == EventOutputState::FAILED);
    CountingSink unused;
    TEST_ASSERT(!book.retry_pending_events(unused));
    TEST_ASSERT(unused.calls == 0);
    TEST_ASSERT(!book.ready_for_commands());
    assert_snapshot_blocked(book);
}

struct ReentrantSink {
    OrderBook& book;
    std::vector<EngineEvent> accepted;

    explicit ReentrantSink(OrderBook& target) : book(target) {}

    bool push(const EngineEvent& event) {
        const auto before = book.capture_state();
        const Command next = CancelOrderCommand(CommandSequence(2), OrderId(1));
        CountingSink nested;
        assert_command_blocked(book.process(next));
        assert_command_blocked(book.process(next, nested));
        TEST_ASSERT(!book.retry_pending_events(nested));
        TEST_ASSERT(nested.calls == 0);
        TEST_ASSERT(!book.ready_for_commands());
        TEST_ASSERT(book.capture_state() == before);
        assert_snapshot_blocked(book);
        accepted.push_back(event);
        return true;
    }
};

void test_sink_reentrancy_cannot_mutate_or_redeliver_a_batch() {
    for (bool start_pending : {false, true}) {
        OrderBook book(4);
        const Command order = NewOrderCommand(CommandSequence(1), OrderId(1), Side::BUY,
            from_double(100), Quantity(1));
        ReentrantSink sink{book};
        if (start_pending) {
            FixedEventBuffer<0> none;
            TEST_ASSERT(!book.process(order, none).event_output_complete);
            TEST_ASSERT(book.retry_pending_events(sink));
        } else {
            TEST_ASSERT(book.process(order, sink).event_output_complete);
        }
        TEST_ASSERT(sink.accepted.size() == 1);
        TEST_ASSERT(book.ready_for_commands());
        TEST_ASSERT(book.last_applied_command_sequence() == CommandSequence(1));
        TEST_ASSERT(book.active_order_count() == 1 && book.check_invariants());
        NullEventSink next_sink;
        TEST_ASSERT(book.process(CancelOrderCommand(CommandSequence(2), OrderId(1)),
            next_sink).applied());
    }
}

void test_max_sweep_and_residual_fit_preallocated_batch() {
    for (TimeInForce tif : {TimeInForce::GTC, TimeInForce::IOC}) {
        // Three resting orders + one reserved aggressive slot bound the
        // command to three TradeEvents plus one Rested/Cancelled event.
        OrderBook book(4, true, {}, 0);
        const size_t capacity_before = book.event_log_capacity();
        TEST_ASSERT(capacity_before >= 4);
        ReferenceEngine reference(4);
        seed_three_asks(book, reference);
        std::vector<EngineEvent> expected;
        reference.process(sweep_command(tif), expected);
        VectorEventSink sink;
        const auto result = book.process(sweep_command(tif), sink);
        TEST_ASSERT(result.applied() && result.event_output_complete);
        TEST_ASSERT(result.event_count == 4);
        TEST_ASSERT(sink.events() == expected);
        TEST_ASSERT(book.event_log_capacity() == capacity_before);
        TEST_ASSERT(book.engine_events().empty());
        TEST_ASSERT(book.ready_for_commands());
        TEST_ASSERT(book.capture_state() == reference.capture_state());
        TEST_ASSERT(book.check_invariants());
    }
}

void test_events_disabled_do_not_call_or_block_on_sink() {
    OrderBook book(4, false);
    ThrowingSink never_called{true};
    const auto result = book.process(NewOrderCommand(CommandSequence(1), OrderId(1),
        Side::BUY, from_double(100), Quantity(1)), never_called);
    TEST_ASSERT(result.applied() && result.event_output_complete);
    TEST_ASSERT(result.event_count == 0);
    TEST_ASSERT(never_called.calls == 0);
    TEST_ASSERT(book.event_output_state() == EventOutputState::READY);
    TEST_ASSERT(book.pending_event_count() == 0 && book.ready_for_commands());
    TEST_ASSERT(book.engine_events().empty());
    TEST_ASSERT(book.create_snapshot().active_orders.size() == 1);
    TEST_ASSERT(book.retry_pending_events(never_called));
    TEST_ASSERT(never_called.calls == 0);
    FixedEventBuffer<0> none;
    TEST_ASSERT(book.process(CancelOrderCommand(CommandSequence(2), OrderId(1)), none).applied());
    TEST_ASSERT(book.active_order_count() == 0 && book.check_invariants());
}

} // namespace

void test_legacy_capture_is_bounded_and_reports_overflow() {
    OrderBook book(2, true, PriceLadderConfig{}, 1);
    const ProcessResult first = book.process(NewOrderCommand(
        CommandSequence(1), OrderId(1), Side::BUY, from_double(99), Quantity(1)));
    const ProcessResult second = book.process(NewOrderCommand(
        CommandSequence(2), OrderId(2), Side::BUY, from_double(98), Quantity(1)));
    const ProcessResult third = book.process(NewOrderCommand(
        CommandSequence(3), OrderId(3), Side::BUY, from_double(97), Quantity(1)));
    TEST_ASSERT(first.event_output_complete && second.event_output_complete);
    TEST_ASSERT(!third.event_output_complete);
    TEST_ASSERT(book.engine_events().size() == 2);
    TEST_ASSERT(book.engine_events().capacity() == 2);
}

void test_frozen_dispatcher_rejects_new_subscriptions() {
    EventDispatcher dispatcher;
    size_t calls = 0;
    auto subscription = dispatcher.subscribe_all(
        [&](const SystemEvent&) { ++calls; });
    dispatcher.freeze();
    TEST_ASSERT(dispatcher.frozen());
    bool subscribe_rejected = false;
    try {
        auto rejected = dispatcher.subscribe_all([](const SystemEvent&) {});
        (void)rejected;
    } catch (const std::logic_error&) {
        subscribe_rejected = true;
    }
    TEST_ASSERT(subscribe_rejected);
    const SystemEvent event = SystemEvent::market_data(Timestamp(1), Symbol("AAPL"),
        MarketDataEvent{Price(1), Quantity(1), Price(1), Price(1)});
    dispatcher.publish(event);
    TEST_ASSERT(calls == 1);
    subscription.reset();
    dispatcher.publish(event);
    TEST_ASSERT(calls == 1);
    TEST_ASSERT(dispatcher.all_event_subscriber_count() == 0);
}

int main() {
    try {
        test_dispatch_order_and_filtering();
        test_order_to_trade_event_flow();
        test_cancel_and_reject_mapping();
        test_market_data_does_not_mutate_book();
        test_subscription_lifetime();
        test_unsubscribe_during_dispatch_affects_next_event();
        test_v2_sink_drains_order_book_history();
        test_fixed_sink_reports_overflow();
        test_pending_suffix_retries_are_ordered_and_block_both_inputs();
        test_zero_capacity_sink_retains_the_entire_batch();
        test_move_preserves_pending_batch_and_delivery_cursor();
        test_sink_exceptions_fail_closed_without_ambiguous_retries();
        test_exception_while_retrying_pending_batch_is_terminal();
        test_sink_reentrancy_cannot_mutate_or_redeliver_a_batch();
        test_max_sweep_and_residual_fit_preallocated_batch();
        test_events_disabled_do_not_call_or_block_on_sink();
        test_legacy_capture_is_bounded_and_reports_overflow();
        test_frozen_dispatcher_rejects_new_subscriptions();
        std::cout << "Event system tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Event system tests failed: " << error.what() << '\n';
        return 1;
    }
}
