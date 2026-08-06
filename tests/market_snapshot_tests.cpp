#include "market_snapshot.hpp"
#include "journal.hpp"
#include "replay_v2.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

#define TEST_ASSERT(condition) do { if (!(condition)) \
    throw std::runtime_error("Assertion failed: " #condition); } while (false)

namespace {

// Recovery tests deliberately use all three default instruments so the
// manifest commit is verified as one multi-book generation.

void submit_prefix(TradingEngine& engine) {
    engine.submit(Timestamp(1), Symbol("AAPL"),
        NewOrderCommand(CommandSequence(1), OrderId(1), Side::SELL,
                        from_double(100), Quantity(10)));
    engine.submit(Timestamp(2), Symbol("TSLA"),
        NewOrderCommand(CommandSequence(1), OrderId(1), Side::BUY,
                        from_double(200), Quantity(8)));
    engine.submit(Timestamp(3), Symbol("NVDA"),
        NewOrderCommand(CommandSequence(1), OrderId(1), Side::BUY,
                        from_double(300), Quantity(12)));
}

void submit_suffix(TradingEngine& engine) {
    engine.submit(Timestamp(4), Symbol("AAPL"),
        NewOrderCommand(CommandSequence(2), OrderId(2), Side::BUY,
                        from_double(100), Quantity(4)));
    engine.submit(Timestamp(5), Symbol("TSLA"),
        CancelOrderCommand(CommandSequence(2), OrderId(1)));
    engine.submit(Timestamp(6), Symbol("NVDA"),
        NewOrderCommand(CommandSequence(2), OrderId(2), Side::SELL,
                        from_double(300), Quantity(5)));
}

void remove_generation(const std::filesystem::path& manifest, uint64_t sequence,
                       size_t symbols) {
    for (size_t index = 0; index < symbols; ++index) {
        const auto filename = manifest.filename().string() + ".g" +
            std::to_string(sequence) + "." + std::to_string(index) + ".book";
        std::filesystem::remove(manifest.parent_path() / filename);
    }
    std::filesystem::remove(manifest);
}

void test_multi_symbol_snapshot_and_incremental_journal_replay() {
    const auto manifest = std::filesystem::temp_directory_path() /
                          "matching_engine.market.snapshot";
    MarketManager source_markets(64);
    EventDispatcher source_dispatcher;
    Journal journal;
    journal.attach(source_dispatcher);
    TradingEngine source_engine(source_markets, source_dispatcher);
    submit_prefix(source_engine);
    TEST_ASSERT(!journal.records().empty());
    const uint64_t snapshot_sequence = journal.records().back().sequence;
    MarketSnapshotStore::save_atomic(manifest, snapshot_sequence, source_markets);
    submit_suffix(source_engine);

    MarketSnapshotRecoveryResult recovered = MarketSnapshotStore::load(manifest);
    TEST_ASSERT(recovered.success());
    TEST_ASSERT(recovered.snapshot_sequence == snapshot_sequence);
    TEST_ASSERT(recovered.markets->symbol_count() == 3);

    std::vector<JournalRecord> incremental;
    for (const JournalRecord& record : journal.records()) {
        if (record.sequence > snapshot_sequence) incremental.push_back(record);
    }
    EventDispatcher recovery_dispatcher;
    TradingEngine recovery_engine(*recovered.markets, recovery_dispatcher);
    const JournalReplayResult replay = JournalReplayEngine::replay(
        incremental, recovery_engine, recovery_dispatcher, &source_markets);
    TEST_ASSERT(replay.success());
    for (const Symbol& symbol : source_markets.symbols()) {
        TEST_ASSERT(source_markets.find_book(symbol)->capture_state() ==
                    recovered.markets->find_book(symbol)->capture_state());
    }
    remove_generation(manifest, snapshot_sequence, 3);
}

void test_manifest_corruption_does_not_expose_partial_manager() {
    const auto manifest = std::filesystem::temp_directory_path() /
                          "matching_engine_bad.market.snapshot";
    MarketManager markets(16);
    MarketSnapshotStore::save_atomic(manifest, 9, markets);
    {
        std::fstream file(manifest, std::ios::binary | std::ios::in | std::ios::out);
        char byte = 0;
        file.read(&byte, 1);
        byte ^= 0x01;
        file.seekp(0);
        file.write(&byte, 1);
    }
    const MarketSnapshotRecoveryResult result = MarketSnapshotStore::load(manifest);
    TEST_ASSERT(!result.success());
    TEST_ASSERT(result.markets == nullptr);
    remove_generation(manifest, 9, 3);
}

} // namespace

int main() {
    try {
        test_multi_symbol_snapshot_and_incremental_journal_replay();
        test_manifest_corruption_does_not_expose_partial_manager();
        std::cout << "Market snapshot tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Market snapshot tests failed: " << error.what() << '\n';
        return 1;
    }
}
