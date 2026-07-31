#include "data_validator.hpp"
#include "example_strategy.hpp"
#include <iostream>
#include <stdexcept>
#include <unordered_map>

#define TEST_ASSERT(condition)                                                \
    do {                                                                      \
        if (!(condition)) {                                                   \
            throw std::runtime_error("Assertion failed: " #condition);       \
        }                                                                     \
    } while (false)

void test_strategy_uses_normal_order_path() {
    MarketManager markets(64);
    EventDispatcher dispatcher;
    TradingEngine engine(markets, dispatcher);
    StrategyHost host(engine);
    uint64_t next_order_id = 100;
    std::unordered_map<std::string, uint64_t> sequences;
    BuyBelowAskStrategy strategy(from_double(101.00), Quantity(5),
        [&](const Symbol& symbol) {
            return CommandSequence(++sequences[symbol.value()]);
        },
        [&] { return OrderId(next_order_id++); });
    host.add_strategy(strategy);
    host.attach(dispatcher);

    size_t order_events = 0;
    auto order_subscription = dispatcher.subscribe(SystemEventType::ORDER,
        [&](const SystemEvent&) { ++order_events; });
    (void)order_subscription;

    engine.publish_market_data(Timestamp(1), Symbol("AAPL"),
        MarketDataEvent{from_double(100), Quantity(10),
                        from_double(99.99), from_double(100.01)});
    TEST_ASSERT(order_events == 1);
    TEST_ASSERT(markets.find_book(Symbol("AAPL"))->best_bid() == from_double(100.01));
    TEST_ASSERT(markets.find_book(Symbol("AAPL"))->active_order_count() == 1);

    // One-shot per symbol: another qualifying tick does not duplicate the order.
    engine.publish_market_data(Timestamp(2), Symbol("AAPL"),
        MarketDataEvent{from_double(100), Quantity(10),
                        from_double(99.99), from_double(100.00)});
    TEST_ASSERT(order_events == 1);

    // Symbol state is independent.
    engine.publish_market_data(Timestamp(3), Symbol("TSLA"),
        MarketDataEvent{from_double(100), Quantity(10),
                        from_double(99.99), from_double(100.02)});
    TEST_ASSERT(order_events == 2);
    TEST_ASSERT(markets.find_book(Symbol("TSLA"))->active_order_count() == 1);
}

void test_rejected_market_data_never_reaches_strategy() {
    MarketManager markets(32);
    EventDispatcher dispatcher;
    TradingEngine engine(markets, dispatcher);
    StrategyHost host(engine);
    uint64_t strategy_calls = 0;

    class CountingStrategy : public Strategy {
        uint64_t& calls_;
    public:
        explicit CountingStrategy(uint64_t& calls) : calls_(calls) {}
        void on_market_data(Timestamp, const Symbol&, const MarketDataEvent&,
                            StrategyContext&) override { ++calls_; }
    } strategy(strategy_calls);

    host.add_strategy(strategy);
    host.attach(dispatcher);
    DataValidator validator(dispatcher);
    const ParsedMarketData crossed{Timestamp(1), Symbol("NVDA"),
        MarketDataEvent{from_double(150), Quantity(10),
                        from_double(151), from_double(150)}};
    if (validator.validate(crossed)) {
        engine.publish_market_data(crossed.timestamp, crossed.symbol, crossed.data);
    }
    TEST_ASSERT(strategy_calls == 0);
    TEST_ASSERT(markets.find_book(Symbol("NVDA"))->active_order_count() == 0);
}

int main() {
    try {
        test_strategy_uses_normal_order_path();
        test_rejected_market_data_never_reaches_strategy();
        std::cout << "Strategy tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Strategy tests failed: " << error.what() << '\n';
        return 1;
    }
}
