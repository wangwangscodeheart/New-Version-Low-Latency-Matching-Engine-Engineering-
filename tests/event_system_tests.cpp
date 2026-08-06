#include "trading_engine.hpp"
#include <iostream>
#include <stdexcept>
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
    TEST_ASSERT(book.engine_events().empty());
    TEST_ASSERT(book.active_order_count() == 0);
    TEST_ASSERT(book.check_invariants());
}

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
        test_legacy_capture_is_bounded_and_reports_overflow();
        test_frozen_dispatcher_rejects_new_subscriptions();
        std::cout << "Event system tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Event system tests failed: " << error.what() << '\n';
        return 1;
    }
}
