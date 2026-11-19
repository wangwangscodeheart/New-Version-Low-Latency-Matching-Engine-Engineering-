#ifndef JOURNAL_HPP
#define JOURNAL_HPP

#include "event_dispatcher.hpp"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
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
    uint8_t format_version = 1;
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

inline void write_command_journal_header(std::ostream& output) {
    output << "DME_JOURNAL,2,COMMAND_BATCHES\n";
    write_journal_header(output);
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
            if (text.empty() || !std::all_of(text.begin(), text.end(),
                    [](char value) { return value >= '0' && value <= '9'; })) {
                throw std::invalid_argument("unsigned decimal required");
            }
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

    static std::vector<std::string> split_fields(const std::string& line) {
        std::vector<std::string> parts;
        size_t begin = 0;
        for (;;) {
            const size_t comma = line.find(',', begin);
            parts.push_back(line.substr(begin, comma == std::string::npos ?
                std::string::npos : comma - begin));
            if (comma == std::string::npos) return parts;
            begin = comma + 1;
        }
    }

    static JournalRecord parse_record(const std::string& line, size_t line_number,
                                      bool command_format) {
        const auto parts = split_fields(line);
        if ((command_format && parts.size() != 13) ||
            (!command_format && parts.size() != 11 && parts.size() != 12 &&
             parts.size() != 13)) {
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
        if (parts[3].empty()) {
            throw std::runtime_error("Invalid journal symbol at line " +
                                     std::to_string(line_number));
        }
        CommandOutcome outcome = CommandOutcome::APPLIED;
        if (parts.size() >= 12) {
            const uint64_t parsed = parse_u64(parts[11], line_number);
            if (parsed > static_cast<uint64_t>(CommandOutcome::SYSTEM_UNAVAILABLE)) {
                throw std::runtime_error("Invalid command outcome at line " +
                                         std::to_string(line_number));
            }
            outcome = static_cast<CommandOutcome>(parsed);
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
        record.format_version = command_format ? 2 : 1;
        return record;
    }

    // Only a prefix of a valid row can be a torn final write. A fully present
    // malformed field (even at EOF) is corruption, not a recovery boundary.
    static bool truncated_command_row(const std::string& line, size_t line_number) {
        const auto parts = split_fields(line);
        if (parts.size() > 13) return false;
        const auto token_valid = [line_number](size_t field,
                                               const std::string& token,
                                               bool partial) {
            const auto matches = [&token, partial](const char* expected) {
                const std::string value(expected);
                return partial ? value.compare(0, token.size(), token) == 0 :
                                 value == token;
            };
            if (field == 2) return matches("ORDER_INSERT") || matches("CANCEL") ||
                matches("TRADE") || matches("COMMAND_RESULT");
            if (field == 3) return partial || !token.empty();
            if (field == 6) return matches("BUY") || matches("SELL");
            if (field == 12) return matches("GTC") || matches("IOC") ||
                matches("POST_ONLY");
            if (partial && token.empty()) return true;
            if (field == 7) {
                if (partial && (token == "-" || token == "+")) return true;
                (void)parse_i64(token, line_number);
                return true;
            }
            const uint64_t value = parse_u64(token, line_number);
            return field != 11 ||
                value <= static_cast<uint64_t>(CommandOutcome::SYSTEM_UNAVAILABLE);
        };
        try {
            for (size_t i = 0; i < parts.size(); ++i) {
                if (!token_valid(i, parts[i], i + 1 == parts.size())) return false;
            }
        } catch (const std::runtime_error&) {
            return false;
        }
        return true;
    }

    // Shared by streaming loading and vector replay preflight. This is an
    // in-process complete-command boundary, not a CRC/fsync durability claim.
    class CommittedRecordValidator {
        std::optional<JournalRecord> input_;
        uint64_t previous_sequence_ = 0;
        uint64_t previous_timestamp_ = 0;
        bool have_previous_ = false;
        bool have_trade_ = false;

    public:
        bool accept(const JournalRecord& record) {
            if (record.format_version != 2) {
                throw std::runtime_error("Command journal contains a non-v2 record");
            }
            if ((record.side != Side::BUY && record.side != Side::SELL) ||
                (record.time_in_force != TimeInForce::GTC &&
                 record.time_in_force != TimeInForce::IOC &&
                 record.time_in_force != TimeInForce::POST_ONLY) ||
                static_cast<unsigned>(record.outcome) >
                    static_cast<unsigned>(CommandOutcome::SYSTEM_UNAVAILABLE)) {
                throw std::runtime_error("Invalid command journal enum value");
            }
            if (record.sequence == 0 || (have_previous_ &&
                (previous_sequence_ == std::numeric_limits<uint64_t>::max() ||
                 record.sequence != previous_sequence_ + 1))) {
                throw std::runtime_error("Command journal sequence is not contiguous");
            }
            if (have_previous_ && record.timestamp.get() < previous_timestamp_) {
                throw std::runtime_error("Journal timestamp moved backwards");
            }
            bool complete = false;
            if (!input_) {
                if (record.type != JournalRecordType::ORDER_INSERT &&
                    record.type != JournalRecordType::CANCEL) {
                    throw std::runtime_error("Command journal group must start with an input");
                }
                input_ = record;
                have_trade_ = false;
            } else {
                if (record.timestamp != input_->timestamp || record.symbol != input_->symbol ||
                    record.command_sequence != input_->command_sequence) {
                    throw std::runtime_error("Command journal group identity mismatch");
                }
                if (record.type == JournalRecordType::TRADE) {
                    if (input_->type != JournalRecordType::ORDER_INSERT ||
                        record.aggressive_order_id != input_->order_id) {
                        throw std::runtime_error("Command journal trade does not match input");
                    }
                    have_trade_ = true;
                } else if (record.type == JournalRecordType::COMMAND_RESULT) {
                    if (record.order_id != input_->order_id ||
                        record.outcome == CommandOutcome::SYSTEM_UNAVAILABLE ||
                        static_cast<unsigned>(record.outcome) >
                            static_cast<unsigned>(CommandOutcome::SYSTEM_UNAVAILABLE) ||
                        (have_trade_ && record.outcome != CommandOutcome::APPLIED)) {
                        throw std::runtime_error("Invalid committed command result");
                    }
                    input_.reset();
                    complete = true;
                } else {
                    throw std::runtime_error("Command journal input has no result");
                }
            }
            previous_sequence_ = record.sequence;
            previous_timestamp_ = record.timestamp.get();
            have_previous_ = true;
            return complete;
        }

        bool pending() const noexcept { return input_.has_value(); }
    };

public:
    // Complete suffixes may start at any nonzero record sequence. File loading
    // separately requires sequence 1, because it reads a whole journal.
    static void validate_committed_records(const std::vector<JournalRecord>& records) {
        CommittedRecordValidator validator;
        for (const auto& record : records) validator.accept(record);
        if (validator.pending()) {
            throw std::runtime_error("Incomplete final command journal group");
        }
    }

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
        file.flush();
        if (!file) throw std::runtime_error("Cannot write journal: " + filename);
    }

    static std::vector<JournalRecord> load(const std::string& filename,
                                           bool ignore_incomplete_tail = false) {
        std::ifstream file(filename);
        if (!file) throw std::runtime_error("Cannot open journal: " + filename);
        std::vector<JournalRecord> result;
        std::string line;
        size_t line_number = 0;
        bool command_format = false;
        size_t committed_size = 0;
        CommittedRecordValidator validator;
        while (std::getline(file, line)) {
            ++line_number;
            const bool terminated = !file.eof();
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line_number == 1 && line == "DME_JOURNAL,2,COMMAND_BATCHES") {
                command_format = true;
                std::ostringstream expected_header;
                write_journal_header(expected_header);
                std::string expected = expected_header.str();
                expected.pop_back();
                if (!terminated || !std::getline(file, line)) {
                    throw std::runtime_error("Missing command journal column header");
                }
                ++line_number;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line != expected || file.eof()) {
                    throw std::runtime_error("Invalid command journal column header");
                }
                continue;
            }
            if (line_number == 1 && line.rfind("DME_JOURNAL,", 0) == 0) {
                throw std::runtime_error("Unsupported command journal format marker");
            }
            if (line_number == 1 && line.rfind("sequence,", 0) == 0) continue;
            if (!command_format && line.empty()) continue;
            JournalRecord record;
            try {
                record = parse_record(line, line_number, command_format);
            } catch (const std::runtime_error&) {
                if (ignore_incomplete_tail && !terminated &&
                    (!command_format || truncated_command_row(line, line_number))) {
                    if (command_format) result.resize(committed_size);
                    break;
                }
                throw;
            }
            if (command_format) {
                if (result.empty() && record.sequence != 1) {
                    throw std::runtime_error("Command journal must start at sequence 1");
                }
                // Validate even a parseable unterminated row so semantic
                // corruption cannot be disguised as a recoverable torn write.
                const bool complete = validator.accept(record);
                if (!terminated) {
                    if (!ignore_incomplete_tail) {
                        throw std::runtime_error("Unterminated command journal row");
                    }
                    result.resize(committed_size);
                    break;
                }
                result.push_back(std::move(record));
                if (complete) committed_size = result.size();
            } else {
                result.push_back(std::move(record));
            }
        }
        if (file.bad()) throw std::runtime_error("Cannot read journal: " + filename);
        if (command_format) {
            if (result.size() != committed_size) {
                if (!ignore_incomplete_tail) {
                    throw std::runtime_error("Incomplete final command journal group");
                }
                result.resize(committed_size);
            }
            return result;
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
