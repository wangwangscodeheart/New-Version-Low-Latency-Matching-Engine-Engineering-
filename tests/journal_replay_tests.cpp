#include "audit_log.hpp"
#include "journal.hpp"
#include "replay_v2.hpp"
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

void run_scenario(TradingEngine& engine) {
    engine.submit(Timestamp(100), Symbol("AAPL"),
        NewOrderCommand(CommandSequence(1), OrderId(1), Side::SELL,
                        from_double(100), Quantity(10)));
    engine.submit(Timestamp(101), Symbol("TSLA"),
        NewOrderCommand(CommandSequence(1), OrderId(1), Side::BUY,
                        from_double(200), Quantity(20)));
    engine.submit(Timestamp(102), Symbol("AAPL"),
        NewOrderCommand(CommandSequence(2), OrderId(2), Side::BUY,
                        from_double(100), Quantity(4)));
    engine.submit(Timestamp(103), Symbol("AAPL"),
        CancelOrderCommand(CommandSequence(3), OrderId(1)));
}

void test_journal_roundtrip_and_replay() {
    MarketManager source_markets(64);
    EventDispatcher source_dispatcher;
    Journal journal;
    journal.attach(source_dispatcher);
    TradingEngine source(source_markets, source_dispatcher);
    run_scenario(source);

    TEST_ASSERT(journal.records().size() == 9);
    TEST_ASSERT(journal.records()[0].type == JournalRecordType::ORDER_INSERT);
    TEST_ASSERT(journal.records()[1].type == JournalRecordType::COMMAND_RESULT);
    TEST_ASSERT(journal.records()[5].type == JournalRecordType::TRADE);
    TEST_ASSERT(journal.records()[7].type == JournalRecordType::CANCEL);

    const auto path = std::filesystem::temp_directory_path() / "matching_v2.journal.csv";
    journal.save(path.string());
    const std::vector<JournalRecord> loaded = Journal::load(path.string());
    std::filesystem::remove(path);
    TEST_ASSERT(loaded.size() == journal.records().size());

    MarketManager replay_markets(64);
    EventDispatcher replay_dispatcher;
    TradingEngine replay_engine(replay_markets, replay_dispatcher);
    const JournalReplayResult result = JournalReplayEngine::replay(
        loaded, replay_engine, replay_dispatcher, &source_markets);
    TEST_ASSERT(result.success());
    TEST_ASSERT(result.replayed_commands == 4);
    TEST_ASSERT(result.expected_outcomes == 4);
    TEST_ASSERT(result.actual_outcomes == 4);
    TEST_ASSERT(result.symbols.size() == 3);
    TEST_ASSERT(source_markets.find_book(Symbol("AAPL"))->capture_state() ==
                replay_markets.find_book(Symbol("AAPL"))->capture_state());
    TEST_ASSERT(source_markets.find_book(Symbol("TSLA"))->capture_state() ==
                replay_markets.find_book(Symbol("TSLA"))->capture_state());
    TEST_ASSERT(source_markets.find_book(Symbol("NVDA"))->capture_state() ==
                replay_markets.find_book(Symbol("NVDA"))->capture_state());
}

void test_replay_reports_first_outcome_mismatch() {
    MarketManager source_markets(32);
    EventDispatcher source_dispatcher;
    Journal journal;
    journal.attach(source_dispatcher);
    TradingEngine source(source_markets, source_dispatcher);
    source.submit(Timestamp(1), Symbol("MSFT"),
        NewOrderCommand(CommandSequence(1), OrderId(1), Side::BUY,
                        from_double(100), Quantity(1)));

    std::vector<JournalRecord> corrupt = journal.records();
    TEST_ASSERT(corrupt.size() == 2);
    TEST_ASSERT(corrupt[1].type == JournalRecordType::COMMAND_RESULT);
    const uint64_t expected_mismatch = corrupt[1].sequence;
    corrupt[1].outcome = CommandOutcome::APPLIED;

    MarketManager replay_markets(32);
    EventDispatcher replay_dispatcher;
    TradingEngine replay_engine(replay_markets, replay_dispatcher);
    const JournalReplayResult result = JournalReplayEngine::replay(
        corrupt, replay_engine, replay_dispatcher, &source_markets);
    TEST_ASSERT(!result.success());
    TEST_ASSERT(!result.outcomes_equal);
    TEST_ASSERT(result.unknown_symbols == 1);
    TEST_ASSERT(result.rejected_commands == 1);
    TEST_ASSERT(result.first_mismatched_journal_sequence == expected_mismatch);
}

void test_audit_lifecycle() {
    MarketManager markets(64);
    EventDispatcher dispatcher;
    AuditLog audit;
    audit.attach(dispatcher);
    TradingEngine engine(markets, dispatcher);
    run_scenario(engine);

    const auto& records = audit.records();
    TEST_ASSERT(records.size() == 9);
    TEST_ASSERT(records[0].action == AuditAction::CREATE);
    TEST_ASSERT(records[1].action == AuditAction::SUBMIT);

    bool passive_partial = false;
    bool aggressive_filled = false;
    bool cancelled = false;
    for (const AuditRecord& record : records) {
        if (record.action == AuditAction::TRADE && record.order_id == OrderId(1) &&
            record.symbol == Symbol("AAPL")) {
            passive_partial = record.status == AuditStatus::PARTIALLY_FILLED &&
                              record.remaining_quantity == Quantity(6);
        }
        if (record.action == AuditAction::TRADE && record.order_id == OrderId(2)) {
            aggressive_filled = record.status == AuditStatus::FILLED;
        }
        if (record.action == AuditAction::CANCEL) {
            cancelled = record.order_id == OrderId(1) &&
                        record.quantity == Quantity(6);
        }
    }
    TEST_ASSERT(passive_partial);
    TEST_ASSERT(aggressive_filled);
    TEST_ASSERT(cancelled);
}

void test_audit_rejection_lifecycle() {
    MarketManager markets(64);
    EventDispatcher dispatcher;
    AuditLog audit;
    audit.attach(dispatcher);
    TradingEngine engine(markets, dispatcher);

    engine.submit(Timestamp(1), Symbol("AAPL"),
        NewOrderCommand(CommandSequence(1), OrderId(50), Side::BUY,
                        from_double(100), Quantity(10)));
    engine.submit(Timestamp(2), Symbol("AAPL"),
        NewOrderCommand(CommandSequence(2), OrderId(50), Side::BUY,
                        from_double(100), Quantity(5)));
    engine.submit(Timestamp(3), Symbol("MSFT"),
        NewOrderCommand(CommandSequence(1), OrderId(60), Side::BUY,
                        from_double(100), Quantity(5)));
    engine.submit(Timestamp(4), Symbol("NVDA"),
        NewOrderCommand(CommandSequence(1), OrderId(70), Side::BUY,
                        from_double(150), Quantity(5)));
    engine.submit(Timestamp(5), Symbol("NVDA"),
        NewOrderCommand(CommandSequence(1), OrderId(71), Side::BUY,
                        from_double(150), Quantity(5)));
    engine.submit(Timestamp(6), Symbol("AAPL"),
        CancelOrderCommand(CommandSequence(3), OrderId(50)));

    size_t rejected = 0;
    bool original_cancelled = false;
    for (const AuditRecord& record : audit.records()) {
        if (record.action == AuditAction::REJECT &&
            record.status == AuditStatus::REJECTED) {
            ++rejected;
        }
        if (record.action == AuditAction::CANCEL &&
            record.order_id == OrderId(50) && record.quantity == Quantity(10)) {
            original_cancelled = true;
        }
    }
    TEST_ASSERT(rejected == 3);
    TEST_ASSERT(original_cancelled);
}

void test_corrupt_journal_rejected() {
    const auto path = std::filesystem::temp_directory_path() / "bad_v2.journal.csv";
    {
        std::ofstream file(path);
        file << "sequence,timestamp,type,symbol,command_sequence,order_id,side,price,quantity,passive_id,aggressive_id\n";
        file << "2,100,ORDER_INSERT,AAPL,1,1,BUY,1000000,10,0,0\n";
        file << "1,101,CANCEL,AAPL,2,1,BUY,0,0,0,0\n";
    }
    bool rejected = false;
    try {
        (void)Journal::load(path.string());
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    std::filesystem::remove(path);
    TEST_ASSERT(rejected);
}

int main() {
    try {
        test_journal_roundtrip_and_replay();
        test_replay_reports_first_outcome_mismatch();
        test_audit_lifecycle();
        test_audit_rejection_lifecycle();
        test_corrupt_journal_rejected();
        std::cout << "Journal, audit and replay tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Journal, audit and replay tests failed: " << error.what() << '\n';
        return 1;
    }
}
