#include "market_snapshot.hpp"
#include "journal.hpp"
#include "replay_v2.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#define TEST_ASSERT(condition) do { if (!(condition)) \
    throw std::runtime_error("Assertion failed: " #condition); } while (false)

namespace {

class TemporaryDirectory {
    std::filesystem::path base_, path_;
public:
    TemporaryDirectory() : base_(std::filesystem::weakly_canonical(
                                    std::filesystem::temp_directory_path())) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            const auto candidate = base_ / ("dme_market_snapshot_" + std::to_string(stamp) +
                                            "_" + std::to_string(attempt));
            if (candidate.parent_path() != base_) throw std::runtime_error("Unsafe test directory");
            if (std::filesystem::create_directory(candidate)) { path_ = candidate; return; }
        }
        throw std::runtime_error("Cannot reserve snapshot test directory");
    }
    ~TemporaryDirectory() {
        // Only recursively remove the exact child directory this fixture created.
        try {
            if (!path_.empty() && path_.parent_path() == base_ &&
                path_.filename().string().rfind("dme_market_snapshot_", 0) == 0) {
                std::error_code ignored;
                std::filesystem::remove_all(path_, ignored);
            }
        } catch (...) {}
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    const std::filesystem::path& path() const noexcept { return path_; }
};

std::filesystem::path book_path(const std::filesystem::path& manifest,
                                uint64_t sequence, size_t index) {
    return manifest.parent_path() / (manifest.filename().string() + ".g" +
        std::to_string(sequence) + "." + std::to_string(index) + ".book");
}

std::filesystem::path reservation_path(const std::filesystem::path& manifest, uint64_t sequence) {
    return manifest.parent_path() / (manifest.filename().string() + ".g" +
                                     std::to_string(sequence) + ".reserved");
}

bool save_rejected(const std::filesystem::path& manifest, uint64_t sequence,
                    const MarketManager& markets) {
    try { MarketSnapshotStore::save_atomic(manifest, sequence, markets); }
    catch (const std::runtime_error&) { return true; }
    return false;
}

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read snapshot test evidence");
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad()) throw std::runtime_error("Snapshot test read failed");
    return bytes;
}

std::vector<std::string> generation_bytes(const std::filesystem::path& manifest,
                                         uint64_t sequence) {
    std::vector<std::string> bytes;
    for (size_t index = 0; index < 3; ++index) bytes.push_back(read_bytes(book_path(manifest, sequence, index)));
    return bytes;
}

std::vector<OrderBookState> capture_books(const MarketManager& markets) {
    std::vector<OrderBookState> states;
    for (const Symbol& symbol : markets.symbols()) states.push_back(markets.find_book(symbol)->capture_state());
    return states;
}

bool restored_state_matches(const MarketSnapshotRecoveryResult& restored,
                            const std::vector<OrderBookState>& expected) {
    return restored.success() && capture_books(*restored.markets) == expected;
}

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

void test_multi_symbol_snapshot_and_incremental_journal_replay() {
    TemporaryDirectory directory;
    const auto manifest = directory.path() / "market.snapshot";
    MarketManager source_markets(64);
    EventDispatcher source_dispatcher;
    Journal journal;
    journal.attach(source_dispatcher);
    TradingEngine source_engine(source_markets, source_dispatcher);
    submit_prefix(source_engine);
    TEST_ASSERT(!journal.records().empty());
    const uint64_t snapshot_sequence = journal.records().back().sequence;
    MarketSnapshotStore::save_atomic(manifest, snapshot_sequence, source_markets);
    // Simulate the existing v1 disk layout: loading must not require a marker.
    TEST_ASSERT(std::filesystem::remove(reservation_path(manifest, snapshot_sequence)));
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
}

void test_manifest_corruption_does_not_expose_partial_manager() {
    TemporaryDirectory directory;
    const auto manifest = directory.path() / "bad.market.snapshot";
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
}

void test_reused_generation_preserves_committed_snapshot_on_late_failure() {
    TemporaryDirectory directory;
    const auto manifest = directory.path() / "market.snapshot";
    MarketManager markets(8);
    EventDispatcher dispatcher;
    TradingEngine engine(markets, dispatcher);
    submit_prefix(engine);
    const auto expected = capture_books(markets);
    MarketSnapshotStore::save_atomic(manifest, 40, markets);
    const auto old_manifest = read_bytes(manifest);
    const auto old_books = generation_bytes(manifest, 40);
    submit_suffix(engine);
    // Deterministically fail after the old implementation has replaced every
    // book, but before it can publish a new manifest. No production fault hook.
    auto blocker = manifest;
    blocker += ".tmp";
    TEST_ASSERT(std::filesystem::create_directory(blocker));
    bool save_threw = false;
    try { MarketSnapshotStore::save_atomic(manifest, 40, markets); }
    catch (const std::runtime_error&) { save_threw = true; }
    const bool manifest_unchanged = read_bytes(manifest) == old_manifest;
    const bool books_unchanged = generation_bytes(manifest, 40) == old_books;
    const bool recovered_old_state = restored_state_matches(MarketSnapshotStore::load(manifest), expected);
    std::cout << "reuse generation: save_threw=" << save_threw
              << " manifest_unchanged=" << manifest_unchanged
              << " books_unchanged=" << books_unchanged
              << " recovered_old_state=" << recovered_old_state << '\n';
    TEST_ASSERT(save_threw && manifest_unchanged && books_unchanged && recovered_old_state);
}

void test_reuse_is_rejected_even_without_an_io_failure() {
    TemporaryDirectory directory;
    const auto manifest = directory.path() / "market.snapshot";
    MarketManager markets(8);
    EventDispatcher dispatcher;
    TradingEngine engine(markets, dispatcher);
    submit_prefix(engine);
    MarketSnapshotStore::save_atomic(manifest, 80, markets);
    const auto old_manifest = read_bytes(manifest);
    const auto old_books = generation_bytes(manifest, 80);
    const auto expected = capture_books(markets);
    TEST_ASSERT(save_rejected(manifest, 80, markets)); // Even identical state is not an idempotent retry.
    submit_suffix(engine);
    TEST_ASSERT(save_rejected(manifest, 80, markets));
    TEST_ASSERT(read_bytes(manifest) == old_manifest && generation_bytes(manifest, 80) == old_books);
    TEST_ASSERT(restored_state_matches(MarketSnapshotStore::load(manifest), expected));
    MarketSnapshotStore::save_atomic(manifest, 81, markets);
    TEST_ASSERT(restored_state_matches(MarketSnapshotStore::load(manifest), capture_books(markets)));
    TEST_ASSERT(generation_bytes(manifest, 80) == old_books);
    std::cout << "Generation reuse refused; next generation restores updated state\n";
}

void test_all_legacy_final_and_temporary_paths_are_checked_before_writes() {
    for (bool temporary_collision : {false, true}) {
        TemporaryDirectory directory;
        const auto manifest = directory.path() / "market.snapshot";
        MarketManager markets(4);
        auto collision = book_path(manifest, 50, 2); // Last sorted symbol, no marker.
        if (temporary_collision) collision += ".tmp";
        {
            std::ofstream output(collision, std::ios::binary);
            output << "legacy generation evidence";
            TEST_ASSERT(output.good());
        }
        const auto before = read_bytes(collision);
        TEST_ASSERT(save_rejected(manifest, 50, markets));
        TEST_ASSERT(read_bytes(collision) == before);
        TEST_ASSERT(!std::filesystem::exists(reservation_path(manifest, 50)));
        TEST_ASSERT(!std::filesystem::exists(manifest));
        for (size_t index = 0; index < 2; ++index) {
            const auto path = book_path(manifest, 50, index);
            TEST_ASSERT(!std::filesystem::exists(path) && !std::filesystem::exists(path.string() + ".tmp"));
        }
    }
    std::cout << "Legacy collision preflight passed: last final and temporary files preserved\n";
}

void test_reservation_alone_prevents_generation_reuse() {
    for (bool directory_marker : {true, false}) {
        TemporaryDirectory directory;
        const auto manifest = directory.path() / "market.snapshot";
        const auto reservation = reservation_path(manifest, 51);
        MarketManager markets(4);
        if (directory_marker) TEST_ASSERT(std::filesystem::create_directory(reservation));
        else {
            std::ofstream output(reservation, std::ios::binary);
            output << "occupied";
            TEST_ASSERT(output.good());
        }
        TEST_ASSERT(save_rejected(manifest, 51, markets));
        TEST_ASSERT(!std::filesystem::exists(manifest));
        for (size_t index = 0; index < 3; ++index) {
            const auto path = book_path(manifest, 51, index);
            TEST_ASSERT(!std::filesystem::exists(path) && !std::filesystem::exists(path.string() + ".tmp"));
        }
        TEST_ASSERT(directory_marker ? std::filesystem::is_empty(reservation) : read_bytes(reservation) == "occupied");
    }
    std::cout << "Reservation-only collisions refused without book writes\n";
}

void test_new_generation_late_failure_preserves_old_snapshot_and_consumes_attempt() {
    TemporaryDirectory directory;
    const auto manifest = directory.path() / "market.snapshot";
    MarketManager markets(8);
    EventDispatcher dispatcher;
    TradingEngine engine(markets, dispatcher);
    submit_prefix(engine);
    MarketSnapshotStore::save_atomic(manifest, 60, markets);
    const auto old_manifest = read_bytes(manifest);
    const auto old_books = generation_bytes(manifest, 60);
    const auto expected = capture_books(markets);
    submit_suffix(engine);
    auto blocker = manifest;
    blocker += ".tmp";
    TEST_ASSERT(std::filesystem::create_directory(blocker));
    TEST_ASSERT(save_rejected(manifest, 61, markets));
    TEST_ASSERT(std::filesystem::is_directory(reservation_path(manifest, 61)));
    const auto orphan_books = generation_bytes(manifest, 61); // All files were written before manifest failure.
    TEST_ASSERT(read_bytes(manifest) == old_manifest && generation_bytes(manifest, 60) == old_books);
    TEST_ASSERT(restored_state_matches(MarketSnapshotStore::load(manifest), expected));
    TEST_ASSERT(std::filesystem::remove(blocker)); // Exact empty directory owned by this fixture.
    TEST_ASSERT(save_rejected(manifest, 61, markets));
    TEST_ASSERT(generation_bytes(manifest, 61) == orphan_books);
    MarketSnapshotStore::save_atomic(manifest, 62, markets);
    const auto restored = MarketSnapshotStore::load(manifest);
    TEST_ASSERT(restored.snapshot_sequence == 62 && restored_state_matches(restored, capture_books(markets)));
    TEST_ASSERT(generation_bytes(manifest, 60) == old_books && generation_bytes(manifest, 61) == orphan_books);
    std::cout << "New-generation late failure: old snapshot intact; attempt 61 consumed; 62 committed\n";
}

void test_unloadable_market_shapes_are_rejected_before_any_output() {
    TemporaryDirectory directory;
    const auto manifest = directory.path() / "market.snapshot";
    MarketManager empty(EmptyMarketManagerTag{});
    TEST_ASSERT(save_rejected(manifest, 0, empty) && std::filesystem::is_empty(directory.path()));
    MarketManager zero_capacity(0);
    TEST_ASSERT(save_rejected(manifest, 0, zero_capacity) && std::filesystem::is_empty(directory.path()));
    MarketManager valid(4);
    TEST_ASSERT(save_rejected(directory.path() / ".", 0, valid) && std::filesystem::is_empty(directory.path()));
    MarketSnapshotStore::save_atomic(manifest, 0, valid); // A zero cut itself remains legal.
    TEST_ASSERT(MarketSnapshotStore::load(manifest).success());
    std::cout << "Invalid shapes rejected without consuming generation; zero cut still supported\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--generation-reuse-only") {
            test_reused_generation_preserves_committed_snapshot_on_late_failure();
            return 0;
        }
        if (argc != 1) throw std::runtime_error("Unknown market snapshot test option");
        test_multi_symbol_snapshot_and_incremental_journal_replay();
        test_manifest_corruption_does_not_expose_partial_manager();
        test_reused_generation_preserves_committed_snapshot_on_late_failure();
        test_reuse_is_rejected_even_without_an_io_failure();
        test_all_legacy_final_and_temporary_paths_are_checked_before_writes();
        test_reservation_alone_prevents_generation_reuse();
        test_new_generation_late_failure_preserves_old_snapshot_and_consumes_attempt();
        test_unloadable_market_shapes_are_rejected_before_any_output();
        std::cout << "Market snapshot tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Market snapshot tests failed: " << error.what() << '\n';
        return 1;
    }
}
