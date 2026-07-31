#include "async_journal.hpp"
#include "replay_v2.hpp"
#include "trading_engine.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#define TEST_ASSERT(condition)                                                \
    do {                                                                      \
        if (!(condition)) {                                                   \
            throw std::runtime_error("Assertion failed: " #condition);       \
        }                                                                     \
    } while (false)

void test_async_journal_drain_and_replay() {
    const auto path = std::filesystem::temp_directory_path() / "async_v2.journal.csv";
    MarketManager source_markets(32);
    EventDispatcher source_dispatcher;
    AsyncJournalWriter writer(path.string(), 64);
    writer.attach(source_dispatcher);
    TradingEngine source(source_markets, source_dispatcher);

    source.submit(Timestamp(1), Symbol("AAPL"),
        NewOrderCommand(CommandSequence(1), OrderId(1), Side::SELL,
                        from_double(100), Quantity(10)));
    source.submit(Timestamp(2), Symbol("AAPL"),
        NewOrderCommand(CommandSequence(2), OrderId(2), Side::BUY,
                        from_double(100), Quantity(4)));
    writer.stop();

    TEST_ASSERT(writer.accepted_count() == 5);
    TEST_ASSERT(writer.written_count() == 5);
    TEST_ASSERT(writer.dropped_count() == 0);
    TEST_ASSERT(writer.healthy());

    const std::vector<JournalRecord> records = Journal::load(path.string());
    TEST_ASSERT(records.size() == 5);
    MarketManager replay_markets(32);
    EventDispatcher replay_dispatcher;
    TradingEngine replay_engine(replay_markets, replay_dispatcher);
    const JournalReplayResult replay = JournalReplayEngine::replay(
        records, replay_engine, replay_dispatcher, &source_markets);
    TEST_ASSERT(replay.success());
    std::filesystem::remove(path);
}

void test_incomplete_tail_recovery_only() {
    const auto path = std::filesystem::temp_directory_path() / "truncated_v2.journal.csv";
    {
        std::ofstream file(path);
        write_journal_header(file);
        write_journal_record(file, JournalRecord{1, Timestamp(1),
            JournalRecordType::ORDER_INSERT, Symbol("AAPL"), CommandSequence(1),
            OrderId(1), Side::BUY, from_double(100), Quantity(1),
            OrderId(0), OrderId(0), CommandOutcome::APPLIED});
        file << "2,2,COMMAND_RESULT,AAPL,1"; // simulated torn final write
    }
    bool strict_rejected = false;
    try {
        (void)Journal::load(path.string());
    } catch (const std::runtime_error&) {
        strict_rejected = true;
    }
    TEST_ASSERT(strict_rejected);
    const auto recovered = Journal::load(path.string(), true);
    TEST_ASSERT(recovered.size() == 1);
    std::filesystem::remove(path);
}

void test_middle_corruption_is_never_ignored() {
    const auto path = std::filesystem::temp_directory_path() / "middle_bad_v2.journal.csv";
    {
        std::ofstream file(path);
        write_journal_header(file);
        file << "broken,middle,row\n";
        file << "1,1,ORDER_INSERT,AAPL,1,1,BUY,1000000,1,0,0,0\n";
    }
    bool rejected = false;
    try {
        (void)Journal::load(path.string(), true);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    std::filesystem::remove(path);
    TEST_ASSERT(rejected);
}

int main() {
    try {
        test_async_journal_drain_and_replay();
        test_incomplete_tail_recovery_only();
        test_middle_corruption_is_never_ignored();
        std::cout << "Async journal tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Async journal tests failed: " << error.what() << '\n';
        return 1;
    }
}
