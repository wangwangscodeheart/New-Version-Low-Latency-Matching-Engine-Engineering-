#ifndef REPLAY_V2_HPP
#define REPLAY_V2_HPP

#include "journal.hpp"
#include "trading_engine.hpp"
#include <limits>
#include <memory>
#include <string>
#include <vector>

struct SymbolReplayVerification {
    Symbol symbol;
    bool state_equal;
    bool state_hash_equal;
    uint64_t expected_state_hash;
    uint64_t actual_state_hash;
};

struct JournalReplayResult {
    static constexpr uint64_t NO_MISMATCH = std::numeric_limits<uint64_t>::max();

    size_t replayed_commands = 0;
    size_t expected_trades = 0;
    size_t actual_trades = 0;
    size_t expected_outcomes = 0;
    size_t actual_outcomes = 0;
    size_t rejected_commands = 0;
    size_t unknown_symbols = 0;
    bool trades_equal = false;
    bool outcomes_equal = false;
    bool states_equal = true;
    uint64_t first_mismatched_journal_sequence = NO_MISMATCH;
    std::vector<SymbolReplayVerification> symbols;

    bool success() const noexcept {
        return trades_equal && outcomes_equal && states_equal;
    }
};

class JournalReplayEngine {
    static void note_mismatch(JournalReplayResult& result, uint64_t sequence) {
        if (sequence < result.first_mismatched_journal_sequence) {
            result.first_mismatched_journal_sequence = sequence;
        }
    }

public:
    static JournalReplayResult replay(const std::vector<JournalRecord>& records,
                                      TradingEngine& engine,
                                      EventDispatcher& dispatcher,
                                      const MarketManager* expected_manager = nullptr) {
        auto actual_trades = std::make_shared<std::vector<JournalRecord>>();
        auto actual_outcomes = std::make_shared<std::vector<JournalRecord>>();
        auto trade_subscription = dispatcher.subscribe(SystemEventType::TRADE,
            [actual_trades](const SystemEvent& event) {
                const auto& trade = std::get<TradeEvent>(
                    std::get<EngineEvent>(event.payload));
                actual_trades->push_back(JournalRecord{
                    0, event.timestamp, JournalRecordType::TRADE, event.symbol,
                    trade.command_sequence, OrderId(0), Side::BUY, trade.price,
                    trade.quantity, trade.passive_order_id,
                    trade.aggressive_order_id});
            });
        auto outcome_subscription = dispatcher.subscribe(
            SystemEventType::PROCESSING_LATENCY,
            [actual_outcomes](const SystemEvent& event) {
                const auto& latency = std::get<ProcessingLatencyEvent>(event.payload);
                actual_outcomes->push_back(JournalRecord{
                    0, event.timestamp, JournalRecordType::COMMAND_RESULT,
                    event.symbol, latency.command_sequence, latency.order_id, Side::BUY,
                    Price(0), Quantity(0), OrderId(0), OrderId(0), latency.outcome});
            });
        (void)trade_subscription;
        (void)outcome_subscription;

        JournalReplayResult result;
        std::vector<const JournalRecord*> expected_trades;
        std::vector<const JournalRecord*> expected_outcomes;
        for (const JournalRecord& record : records) {
            if (record.type == JournalRecordType::ORDER_INSERT) {
                engine.submit(record.timestamp, record.symbol,
                    NewOrderCommand(record.command_sequence, record.order_id,
                                    record.side, record.price, record.quantity,
                                    record.time_in_force));
                ++result.replayed_commands;
            } else if (record.type == JournalRecordType::CANCEL) {
                engine.submit(record.timestamp, record.symbol,
                    CancelOrderCommand(record.command_sequence, record.order_id));
                ++result.replayed_commands;
            } else if (record.type == JournalRecordType::TRADE) {
                expected_trades.push_back(&record);
            } else {
                expected_outcomes.push_back(&record);
            }
        }

        result.expected_trades = expected_trades.size();
        result.actual_trades = actual_trades->size();
        result.trades_equal = result.expected_trades == result.actual_trades;
        for (size_t i = 0; i < expected_trades.size(); ++i) {
            if (i >= actual_trades->size()) {
                note_mismatch(result, expected_trades[i]->sequence);
                break;
            }
            const JournalRecord& expected = *expected_trades[i];
            const JournalRecord& actual = (*actual_trades)[i];
            const bool equal = expected.timestamp == actual.timestamp &&
                expected.symbol == actual.symbol &&
                expected.command_sequence == actual.command_sequence &&
                expected.price == actual.price && expected.quantity == actual.quantity &&
                expected.passive_order_id == actual.passive_order_id &&
                expected.aggressive_order_id == actual.aggressive_order_id;
            if (!equal) {
                result.trades_equal = false;
                note_mismatch(result, expected.sequence);
                break;
            }
        }

        result.expected_outcomes = expected_outcomes.size();
        result.actual_outcomes = actual_outcomes->size();
        // Legacy 11-column journals have no outcome records; retain replay
        // compatibility but only claim outcome equality when evidence exists.
        result.outcomes_equal = expected_outcomes.empty() ||
            expected_outcomes.size() == actual_outcomes->size();
        for (size_t i = 0; i < expected_outcomes.size(); ++i) {
            if (i >= actual_outcomes->size()) {
                note_mismatch(result, expected_outcomes[i]->sequence);
                break;
            }
            const JournalRecord& expected = *expected_outcomes[i];
            const JournalRecord& actual = (*actual_outcomes)[i];
            if (actual.outcome != CommandOutcome::APPLIED) ++result.rejected_commands;
            if (actual.outcome == CommandOutcome::UNKNOWN_SYMBOL) ++result.unknown_symbols;
            const bool equal = expected.timestamp == actual.timestamp &&
                expected.symbol == actual.symbol &&
                expected.command_sequence == actual.command_sequence &&
                expected.order_id == actual.order_id &&
                expected.outcome == actual.outcome;
            if (!equal) {
                result.outcomes_equal = false;
                note_mismatch(result, expected.sequence);
                break;
            }
        }

        if (expected_manager) {
            const MarketManager& actual_manager = engine.market_manager();
            for (const Symbol& symbol : expected_manager->symbols()) {
                const OrderBook* expected_book = expected_manager->find_book(symbol);
                const OrderBook* actual_book = actual_manager.find_book(symbol);
                SymbolReplayVerification verification{symbol, false, false,
                    expected_book ? expected_book->state_hash() : 0,
                    actual_book ? actual_book->state_hash() : 0};
                if (expected_book && actual_book) {
                    verification.state_equal =
                        expected_book->capture_state() == actual_book->capture_state();
                    verification.state_hash_equal =
                        verification.expected_state_hash == verification.actual_state_hash;
                }
                result.states_equal = result.states_equal &&
                    verification.state_equal && verification.state_hash_equal;
                result.symbols.push_back(std::move(verification));
            }
        }
        return result;
    }
};

#endif
