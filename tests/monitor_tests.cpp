#include "monitor.hpp"
#include "trading_engine.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#define TEST_ASSERT(condition)                                                \
    do {                                                                      \
        if (!(condition)) {                                                   \
            throw std::runtime_error("Assertion failed: " #condition);       \
        }                                                                     \
    } while (false)

void test_global_and_symbol_metrics() {
    MarketManager markets(64);
    EventDispatcher dispatcher;
    Monitor monitor;
    monitor.attach(dispatcher, &markets);
    dispatcher.freeze();
    TradingEngine engine(markets, dispatcher);

    engine.submit(Timestamp(1), Symbol("AAPL"),
        NewOrderCommand(CommandSequence(1), OrderId(1), Side::SELL,
                        from_double(100), Quantity(10)));
    engine.submit(Timestamp(2), Symbol("AAPL"),
        NewOrderCommand(CommandSequence(2), OrderId(2), Side::BUY,
                        from_double(100), Quantity(4)));
    engine.submit(Timestamp(3), Symbol("AAPL"),
        CancelOrderCommand(CommandSequence(3), OrderId(1)));
    engine.submit(Timestamp(4), Symbol("TSLA"),
        NewOrderCommand(CommandSequence(1), OrderId(3), Side::BUY,
                        Price(0), Quantity(10)));

    const MonitorSnapshot snapshot = monitor.snapshot();
    TEST_ASSERT(snapshot.global.orders == 3);
    TEST_ASSERT(snapshot.global.trades == 1);
    TEST_ASSERT(snapshot.global.cancels == 1);
    TEST_ASSERT(snapshot.global.rejects == 1);
    TEST_ASSERT(snapshot.global.traded_volume == 4);
    TEST_ASSERT(snapshot.global.latency_samples == 4);
    TEST_ASSERT(snapshot.global.max_latency_ns >=
                snapshot.global.average_latency_ns());

    const TradingMetrics& aapl = snapshot.by_symbol.at("AAPL");
    TEST_ASSERT(aapl.orders == 2);
    TEST_ASSERT(aapl.trades == 1);
    TEST_ASSERT(aapl.cancels == 1);
    TEST_ASSERT(aapl.rejects == 0);
    TEST_ASSERT(aapl.traded_volume == 4);
    TEST_ASSERT(aapl.latency_samples == 3);

    const TradingMetrics& tsla = snapshot.by_symbol.at("TSLA");
    TEST_ASSERT(tsla.orders == 1);
    TEST_ASSERT(tsla.rejects == 1);
    TEST_ASSERT(tsla.latency_samples == 1);
}

void test_cli_output_and_logger_metric() {
    MarketManager markets(16);
    EventDispatcher dispatcher;
    Monitor monitor;
    monitor.attach(dispatcher, &markets);
    TradingEngine engine(markets, dispatcher);
    engine.submit(Timestamp(1), Symbol("NVDA"),
        NewOrderCommand(CommandSequence(1), OrderId(1), Side::BUY,
                        from_double(150), Quantity(5)));

    std::ostringstream output;
    monitor.print(output);
    const std::string text = output.str();
    TEST_ASSERT(text.find("Trading System Monitor") != std::string::npos);
    TEST_ASSERT(text.find("Global: orders=1") != std::string::npos);
    TEST_ASSERT(text.find("NVDA: orders=1") != std::string::npos);
    TEST_ASSERT(text.find("dropped_logs=0") != std::string::npos);
}

int main() {
    try {
        test_global_and_symbol_metrics();
        test_cli_output_and_logger_metric();
        std::cout << "Monitor tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Monitor tests failed: " << error.what() << '\n';
        return 1;
    }
}
