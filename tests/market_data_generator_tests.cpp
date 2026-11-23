#include "market_data_generator.hpp"
#include "market_data_loader.hpp"
#include "temporary_test_directory.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <unordered_map>
#include <type_traits>

#define TEST_ASSERT(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

static std::string read_all(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

static void test_fixture_directory_isolation_and_ownership() {
    using matching_engine_test::TemporaryTestDirectory;
    static_assert(!std::is_copy_constructible_v<TemporaryTestDirectory>);
    static_assert(!std::is_move_constructible_v<TemporaryTestDirectory>);
    for (const auto* label : {"", "../escape", "nested/path", "invalid label"}) {
        bool rejected = false;
        try { TemporaryTestDirectory invalid(label); }
        catch (const std::invalid_argument&) { rejected = true; }
        TEST_ASSERT(rejected);
    }
    TemporaryTestDirectory owner("fixture_ownership");
    const auto sentinel = owner.file("same.csv");
    { std::ofstream output(sentinel); output << "owner"; TEST_ASSERT(output.good()); }
    std::filesystem::path other_path;
    {
        TemporaryTestDirectory other("fixture_ownership");
        other_path = other.path();
        TEST_ASSERT(other_path != owner.path());
        const auto other_file = other.file("same.csv");
        { std::ofstream output(other_file); output << "other"; TEST_ASSERT(output.good()); }
        TEST_ASSERT(read_all(sentinel) == "owner" && read_all(other_file) == "other");
        for (const auto* filename : {"", ".", "..", "../escape", "/absolute", "nested/file", "nested\\file"}) {
            bool rejected = false;
            try { (void)other.file(filename); }
            catch (const std::invalid_argument&) { rejected = true; }
            TEST_ASSERT(rejected);
        }
    }
    TEST_ASSERT(!std::filesystem::exists(other_path));
    TEST_ASSERT(read_all(sentinel) == "owner"); // A sibling fixture was never removed.
    struct InjectedFailure {};
    std::filesystem::path throwing_path;
    try {
        TemporaryTestDirectory throwing("fixture_exception");
        throwing_path = throwing.path();
        { std::ofstream output(throwing.file("created.csv")); output << "created"; TEST_ASSERT(output.good()); }
        throw InjectedFailure{};
    } catch (const InjectedFailure&) {}
    TEST_ASSERT(!std::filesystem::exists(throwing_path));

    // Cleanup must not recursively discover/remove unregistered contents.
    std::filesystem::path retained_path, unregistered;
    {
        TemporaryTestDirectory retained("fixture_nonrecursive");
        retained_path = retained.path();
        unregistered = retained.path() / "not_registered.csv";
        { std::ofstream output(unregistered); output << "keep"; TEST_ASSERT(output.good()); }
    }
    TEST_ASSERT(read_all(unregistered) == "keep");
    TEST_ASSERT(std::filesystem::remove(unregistered)); // Exact self-created paths only.
    TEST_ASSERT(std::filesystem::remove(retained_path));
}

int main() {
    try {
        test_fixture_directory_isolation_and_ownership();
        matching_engine_test::TemporaryTestDirectory directory("market_data_generator");
        const auto first = directory.file("synthetic_market_data_first.csv");
        const auto second = directory.file("synthetic_market_data_second.csv");
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
