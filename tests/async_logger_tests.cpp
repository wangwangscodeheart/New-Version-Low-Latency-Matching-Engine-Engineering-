#include "async_logger.hpp"
#include "trading_engine.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#define TEST_ASSERT(condition)                                                \
    do {                                                                      \
        if (!(condition)) {                                                   \
            throw std::runtime_error("Assertion failed: " #condition);       \
        }                                                                     \
    } while (false)

void test_bounded_queue_contract() {
    BoundedQueue<int> queue(2);
    TEST_ASSERT(queue.try_push(1));
    TEST_ASSERT(queue.try_push(2));
    TEST_ASSERT(!queue.try_push(3));
    int value = 0;
    TEST_ASSERT(queue.wait_pop(value) && value == 1);
    TEST_ASSERT(queue.wait_pop(value) && value == 2);
    queue.close();
    TEST_ASSERT(!queue.wait_pop(value));
    TEST_ASSERT(!queue.try_push(4));
}

void test_async_event_logging_and_drain() {
    const auto path = std::filesystem::temp_directory_path() / "matching_async.log";
    MarketManager markets(64);
    EventDispatcher dispatcher;
    AsyncLogger logger(path.string(), 64);
    logger.attach(dispatcher);
    TradingEngine engine(markets, dispatcher);

    engine.submit(Timestamp(100), Symbol("AAPL"),
        NewOrderCommand(CommandSequence(1), OrderId(1), Side::SELL,
                        from_double(100), Quantity(10)));
    engine.submit(Timestamp(101), Symbol("AAPL"),
        NewOrderCommand(CommandSequence(2), OrderId(2), Side::BUY,
                        from_double(100), Quantity(10)));
    engine.publish_market_data(Timestamp(102), Symbol("NVDA"),
        MarketDataEvent{from_double(150), Quantity(500),
                        from_double(149.99), from_double(150.01)});
    logger.stop();

    TEST_ASSERT(logger.accepted_count() == 7);
    TEST_ASSERT(logger.written_count() == logger.accepted_count());
    TEST_ASSERT(logger.dropped_count() == 0);

    std::ifstream file(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) lines.push_back(line);
    file.close();
    std::filesystem::remove(path);
    TEST_ASSERT(lines.size() == 8);
    TEST_ASSERT(lines[1].find("100,ORDER,AAPL,NEW_ORDER") == 0);
    TEST_ASSERT(lines[2].find("100,ORDER_RESTED,AAPL,") == 0);
    TEST_ASSERT(lines[3].find("100,PROCESSING_LATENCY,AAPL,") == 0);
    TEST_ASSERT(lines[5].find("101,TRADE,AAPL,TRADE") == 0);
    TEST_ASSERT(lines[7].find("102,MARKET_DATA,NVDA,") == 0);
}

int main() {
    try {
        test_bounded_queue_contract();
        test_async_event_logging_and_drain();
        std::cout << "Async logger tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Async logger tests failed: " << error.what() << '\n';
        return 1;
    }
}
