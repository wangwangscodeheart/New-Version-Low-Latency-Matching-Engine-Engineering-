#ifndef JOURNAL_HPP
#define JOURNAL_HPP

#include "event_dispatcher.hpp"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <optional>

enum class JournalRecordType : uint8_t {
    ORDER_INSERT = 0,
    CANCEL = 1,
    TRADE = 2,
    COMMAND_RESULT = 3
};

struct JournalRecord {
    uint64_t sequence = 0;
    Timestamp timestamp{0};
    JournalRecordType type = JournalRecordType::ORDER_INSERT;
    Symbol symbol{"UNKNOWN"};
    CommandSequence command_sequence{0};
    OrderId order_id{0};
    Side side = Side::BUY;
    Price price{0};
    Quantity quantity{0};
    OrderId passive_order_id{0};
    OrderId aggressive_order_id{0};
    CommandOutcome outcome = CommandOutcome::APPLIED;
    TimeInForce time_in_force = TimeInForce::GTC;
};

inline const char* journal_record_type_string(JournalRecordType type) noexcept {
    switch (type) {
        case JournalRecordType::ORDER_INSERT: return "ORDER_INSERT";
        case JournalRecordType::CANCEL: return "CANCEL";
        case JournalRecordType::TRADE: return "TRADE";
        case JournalRecordType::COMMAND_RESULT: return "COMMAND_RESULT";
    }
    return "UNKNOWN";
}

inline void write_journal_header(std::ostream& output) {
    output << "sequence,timestamp,type,symbol,command_sequence,order_id,side,price,quantity,passive_id,aggressive_id,outcome,time_in_force\n";
}

inline void write_journal_record(std::ostream& output, const JournalRecord& record) {
    output << record.sequence << ',' << record.timestamp.get() << ','
           << journal_record_type_string(record.type) << ',' << record.symbol.value()
           << ',' << record.command_sequence.get() << ',' << record.order_id.get()
           << ',' << to_string(record.side) << ',' << record.price.get() << ','
           << record.quantity.get() << ',' << record.passive_order_id.get() << ','
           << record.aggressive_order_id.get() << ','
           << static_cast<unsigned>(record.outcome) << ','
           << to_string(record.time_in_force) << '\n';
}

// Stateless event-to-record projection shared by the in-memory Journal and the
// asynchronous writer. Sequence ownership stays with the caller, so projecting
// an event never retains historical records.
class JournalProjector {
public:
    static std::optional<JournalRecord> project(const SystemEvent& event,
                                                 uint64_t sequence) {
        if (event.event_type == SystemEventType::ORDER) {
            const Command& command = std::get<OrderEvent>(event.payload).command;
            if (const auto* order = std::get_if<NewOrderCommand>(&command)) {
                JournalRecord record{sequence, event.timestamp, JournalRecordType::ORDER_INSERT,
                    event.symbol, order->command_sequence, order->order_id, order->side,
                    order->price, order->quantity, OrderId(0), OrderId(0)};
                record.time_in_force = order->time_in_force;
                return record;
            }
            const auto& cancel = std::get<CancelOrderCommand>(command);
            return JournalRecord{sequence, event.timestamp, JournalRecordType::CANCEL,
                event.symbol, cancel.command_sequence, cancel.order_id, Side::BUY,
                Price(0), Quantity(0), OrderId(0), OrderId(0)};
        }
        if (event.event_type == SystemEventType::TRADE) {
            const auto& trade = std::get<TradeEvent>(std::get<EngineEvent>(event.payload));
            return JournalRecord{sequence, event.timestamp, JournalRecordType::TRADE,
                event.symbol, trade.command_sequence, OrderId(0), Side::BUY,
                trade.price, trade.quantity, trade.passive_order_id,
                trade.aggressive_order_id};
        }
        if (event.event_type == SystemEventType::PROCESSING_LATENCY) {
            const auto& latency = std::get<ProcessingLatencyEvent>(event.payload);
            return JournalRecord{sequence, event.timestamp, JournalRecordType::COMMAND_RESULT,
                event.symbol, latency.command_sequence, latency.order_id, Side::BUY,
                Price(0), Quantity(0), OrderId(0), OrderId(0), latency.outcome};
        }
        return std::nullopt;
    }
};

class Journal {
    std::vector<JournalRecord> records_;
    uint64_t next_sequence_ = 1;
    EventDispatcher::Subscription subscription_;

    static uint64_t parse_u64(const std::string& text, size_t line) {
        size_t consumed = 0;
        try {
            const uint64_t value = std::stoull(text, &consumed);
            if (consumed != text.size()) throw std::invalid_argument("trailing");
            return value;
        } catch (...) {
            throw std::runtime_error("Invalid journal integer at line " +
                                     std::to_string(line));
        }
    }

    static int64_t parse_i64(const std::string& text, size_t line) {
        size_t consumed = 0;
        try {
            const int64_t value = std::stoll(text, &consumed);
            if (consumed != text.size()) throw std::invalid_argument("trailing");
            return value;
        } catch (...) {
            throw std::runtime_error("Invalid journal integer at line " +
                                     std::to_string(line));
        }
    }

public:
    // Journal must outlive the dispatcher because the callback captures this.
    void attach(EventDispatcher& dispatcher) {
        subscription_ = dispatcher.subscribe_all(
            [this](const SystemEvent& event) { on_event(event); });
    }

    void on_event(const SystemEvent& event) {
        auto record = JournalProjector::project(event, next_sequence_);
        if (!record) return;
        ++next_sequence_;
        records_.push_back(std::move(*record));
    }

    const std::vector<JournalRecord>& records() const noexcept { return records_; }

    void save(const std::string& filename) const {
        std::ofstream file(filename, std::ios::out | std::ios::trunc);
        if (!file) throw std::runtime_error("Cannot open journal: " + filename);
        write_journal_header(file);
        for (const JournalRecord& record : records_) {
            write_journal_record(file, record);
        }
    }

    static std::vector<JournalRecord> load(const std::string& filename,
                                           bool ignore_incomplete_tail = false) {
        std::ifstream file(filename);
        if (!file) throw std::runtime_error("Cannot open journal: " + filename);
        std::vector<JournalRecord> result;
        std::string line;
        size_t line_number = 0;
        while (std::getline(file, line)) {
            ++line_number;
            if (line_number == 1 && line.rfind("sequence,", 0) == 0) continue;
            if (line.empty()) continue;
            try {
            std::stringstream stream(line);
            std::vector<std::string> parts;
            std::string part;
            while (std::getline(stream, part, ',')) parts.push_back(part);
            if (parts.size() != 11 && parts.size() != 12 && parts.size() != 13) {
                throw std::runtime_error("Invalid journal field count at line " +
                                         std::to_string(line_number));
            }
            JournalRecordType type;
            if (parts[2] == "ORDER_INSERT") type = JournalRecordType::ORDER_INSERT;
            else if (parts[2] == "CANCEL") type = JournalRecordType::CANCEL;
            else if (parts[2] == "TRADE") type = JournalRecordType::TRADE;
            else if (parts[2] == "COMMAND_RESULT") type = JournalRecordType::COMMAND_RESULT;
            else throw std::runtime_error("Invalid journal type at line " +
                                          std::to_string(line_number));
            const Side side = parts[6] == "BUY" ? Side::BUY :
                parts[6] == "SELL" ? Side::SELL :
                throw std::runtime_error("Invalid journal side at line " +
                                         std::to_string(line_number));
            CommandOutcome outcome = CommandOutcome::APPLIED;
            if (parts.size() >= 12) {
                const uint64_t parsed_outcome = parse_u64(parts[11], line_number);
                if (parsed_outcome > static_cast<uint64_t>(CommandOutcome::SYSTEM_UNAVAILABLE)) {
                    throw std::runtime_error("Invalid command outcome at line " +
                                             std::to_string(line_number));
                }
                outcome = static_cast<CommandOutcome>(parsed_outcome);
            }
            TimeInForce time_in_force = TimeInForce::GTC;
            if (parts.size() == 13) {
                time_in_force = parts[12] == "GTC" ? TimeInForce::GTC :
                    parts[12] == "IOC" ? TimeInForce::IOC :
                    parts[12] == "POST_ONLY" ? TimeInForce::POST_ONLY :
                    throw std::runtime_error("Invalid time in force at line " +
                                             std::to_string(line_number));
            }
            JournalRecord record{
                parse_u64(parts[0], line_number), Timestamp(parse_u64(parts[1], line_number)),
                type, Symbol(parts[3]), CommandSequence(parse_u64(parts[4], line_number)),
                OrderId(parse_u64(parts[5], line_number)), side,
                Price(parse_i64(parts[7], line_number)),
                Quantity(parse_u64(parts[8], line_number)),
                OrderId(parse_u64(parts[9], line_number)),
                OrderId(parse_u64(parts[10], line_number)), outcome};
            record.time_in_force = time_in_force;
            result.push_back(std::move(record));
            } catch (const std::runtime_error&) {
                if (ignore_incomplete_tail && file.eof()) break;
                throw;
            }
        }
        uint64_t previous = 0;
        uint64_t previous_timestamp = 0;
        for (const JournalRecord& record : result) {
            if (record.sequence == 0 || record.sequence <= previous) {
                throw std::runtime_error("Journal sequence is not strictly increasing");
            }
            if (record.timestamp.get() < previous_timestamp) {
                throw std::runtime_error("Journal timestamp moved backwards");
            }
            previous = record.sequence;
            previous_timestamp = record.timestamp.get();
        }
        return result;
    }
};

#endif
