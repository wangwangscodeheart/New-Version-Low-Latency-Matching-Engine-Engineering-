#include "market_data_loader.hpp"
#include "monitor.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

#define TEST_ASSERT(condition)                                                \
    do {                                                                      \
        if (!(condition)) {                                                   \
            throw std::runtime_error("Assertion failed: " #condition);       \
        }                                                                     \
    } while (false)

void test_exact_decimal_parser() {
    const ParsedMarketData parsed = MarketDataParser::parse(
        "123,AAPL,190.1234,50,190.1200,190.1300");
    TEST_ASSERT(parsed.timestamp == Timestamp(123));
    TEST_ASSERT(parsed.symbol == Symbol("AAPL"));
    TEST_ASSERT(parsed.data.price == Price(1901234));
    TEST_ASSERT(parsed.data.volume == Quantity(50));
    TEST_ASSERT(parsed.data.bid == Price(1901200));
    TEST_ASSERT(parsed.data.ask == Price(1901300));

    bool rejected_precision = false;
    try {
        (void)MarketDataParser::parse("1,AAPL,1.12345,1,1.0,2.0");
    } catch (const MarketDataParseError&) {
        rejected_precision = true;
    }
    TEST_ASSERT(rejected_precision);
}

void test_loader_validator_and_event_flow() {
    const auto path = std::filesystem::temp_directory_path() / "market_data_v2.csv";
    {
        std::ofstream file(path);
        file << "timestamp,symbol,price,volume,bid,ask\n";
        file << "100,AAPL,100.0000,10,99.9900,100.0100\n";
        file << "101,AAPL,120.0000,10,119.9900,120.0100\n";
        file << "5000,AAPL,120.0000,10,119.9900,120.0100\n";
        file << "4000,AAPL,120.0000,10,119.9900,120.0100\n";
        file << "5001,AAPL,120.0000,10,121.0000,120.0000\n";
        file << "5002,AAPL,,10,119.9900,120.0100\n";
        file << "6000,TSLA,250.0000,20,249.9900,250.0100\n";
    }

    MarketManager markets(32);
    EventDispatcher dispatcher;
    TradingEngine engine(markets, dispatcher);
    Monitor monitor;
    monitor.attach(dispatcher);
    DataValidator validator(dispatcher, DataValidatorConfig{1000, 1000});
    size_t market_events = 0;
    std::vector<DataQualityCode> quality_codes;
    auto market_subscription = dispatcher.subscribe(SystemEventType::MARKET_DATA,
        [&](const SystemEvent&) { ++market_events; });
    auto quality_subscription = dispatcher.subscribe(SystemEventType::DATA_QUALITY,
        [&](const SystemEvent& event) {
            quality_codes.push_back(
                std::get<DataQualityEvent>(event.payload).code);
        });
    (void)market_subscription;
    (void)quality_subscription;

    const MarketDataLoadResult result =
        MarketDataLoader::load(path.string(), validator, engine);
    std::filesystem::remove(path);

    TEST_ASSERT(result.rows == 7);
    TEST_ASSERT(result.published == 4);
    TEST_ASSERT(result.rejected == 3);
    TEST_ASSERT(market_events == 4);
    TEST_ASSERT(quality_codes.size() == 5);
    TEST_ASSERT(quality_codes[0] == DataQualityCode::PRICE_JUMP);
    TEST_ASSERT(quality_codes[1] == DataQualityCode::STALE_SYMBOL);
    TEST_ASSERT(quality_codes[2] == DataQualityCode::TIMESTAMP_BACKWARDS);
    TEST_ASSERT(quality_codes[3] == DataQualityCode::CROSSED_MARKET);
    TEST_ASSERT(quality_codes[4] == DataQualityCode::MISSING_FIELD);

    const MonitorSnapshot snapshot = monitor.snapshot();
    TEST_ASSERT(snapshot.global.data_warnings == 2);
    TEST_ASSERT(snapshot.global.data_errors == 3);
    TEST_ASSERT(snapshot.by_symbol.at("AAPL").data_warnings == 2);
    TEST_ASSERT(snapshot.by_symbol.at("AAPL").data_errors == 2);
    TEST_ASSERT(snapshot.by_symbol.at("UNKNOWN").data_errors == 1);
}

int main() {
    try {
        test_exact_decimal_parser();
        test_loader_validator_and_event_flow();
        std::cout << "Market data tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Market data tests failed: " << error.what() << '\n';
        return 1;
    }
}
