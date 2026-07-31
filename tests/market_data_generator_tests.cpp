#include "market_data_generator.hpp"
#include "market_data_loader.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <unordered_map>

#define TEST_ASSERT(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

static std::string read_all(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

int main() {
    try {
        const auto directory = std::filesystem::temp_directory_path();
        const auto first = directory / "synthetic_market_data_first.csv";
        const auto second = directory / "synthetic_market_data_second.csv";
        MarketDataGeneratorConfig config;
        config.ticks_per_symbol = 10;
        TEST_ASSERT(MarketDataGenerator::generate_csv(first.string(), config) == 30);
        TEST_ASSERT(MarketDataGenerator::generate_csv(second.string(), config) == 30);
        TEST_ASSERT(read_all(first) == read_all(second));

        MarketManager markets(64);
        EventDispatcher dispatcher;
        TradingEngine engine(markets, dispatcher);
        DataValidator validator(dispatcher);
        std::unordered_map<std::string, size_t> counts;
        auto subscription = dispatcher.subscribe(SystemEventType::MARKET_DATA,
            [&](const SystemEvent& event) { ++counts[event.symbol.value()]; });
        const MarketDataLoadResult result =
            MarketDataLoader::load(first.string(), validator, engine);
        TEST_ASSERT(result.rows == 30 && result.published == 30 && result.rejected == 0);
        TEST_ASSERT(counts["AAPL"] == 10 && counts["TSLA"] == 10 && counts["NVDA"] == 10);
        std::filesystem::remove(first);
        std::filesystem::remove(second);
        return 0;
    } catch (const std::exception&) {
        return 1;
    }
}
