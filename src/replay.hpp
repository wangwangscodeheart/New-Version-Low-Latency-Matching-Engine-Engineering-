#ifndef REPLAY_HPP
#define REPLAY_HPP

#include "commands.hpp"
#include "orderbook.hpp"
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

class ReplayEngine {
private:
    static uint64_t parse_u64(const std::string& text, size_t line_number,
                              const char* field) {
        if (text.empty()) {
            throw std::runtime_error("Invalid " + std::string(field) + " at log line " +
                                     std::to_string(line_number));
        }
        for (char ch : text) {
            if (ch < '0' || ch > '9') {
                throw std::runtime_error("Invalid " + std::string(field) + " at log line " +
                                         std::to_string(line_number));
            }
        }
        size_t consumed = 0;
        try {
            const uint64_t value = std::stoull(text, &consumed);
            if (consumed != text.size()) throw std::invalid_argument("trailing characters");
            return value;
        } catch (const std::exception&) {
            throw std::runtime_error("Invalid " + std::string(field) + " at log line " +
                                     std::to_string(line_number));
        }
    }

    static int64_t parse_i64(const std::string& text, size_t line_number,
                             const char* field) {
        const size_t first_digit = (!text.empty() && text[0] == '-') ? 1 : 0;
        if (first_digit == text.size()) {
            throw std::runtime_error("Invalid " + std::string(field) + " at log line " +
                                     std::to_string(line_number));
        }
        for (size_t i = first_digit; i < text.size(); ++i) {
            if (text[i] < '0' || text[i] > '9') {
                throw std::runtime_error("Invalid " + std::string(field) + " at log line " +
                                         std::to_string(line_number));
            }
        }
        size_t consumed = 0;
        try {
            const int64_t value = std::stoll(text, &consumed);
            if (consumed != text.size()) throw std::invalid_argument("trailing characters");
            return value;
        } catch (const std::exception&) {
            throw std::runtime_error("Invalid " + std::string(field) + " at log line " +
                                     std::to_string(line_number));
        }
    }

    static Side parse_side(const std::string& value, size_t line_number) {
        if (value == "BUY") return Side::BUY;
        if (value == "SELL") return Side::SELL;
        throw std::runtime_error("Invalid side at log line " + std::to_string(line_number));
    }

    static EventIndex parse_event_index(const std::string& value, size_t line_number) {
        const uint64_t parsed = parse_u64(value, line_number, "event index");
        if (parsed > std::numeric_limits<uint32_t>::max()) {
            throw std::runtime_error("Invalid event index at log line " +
                                     std::to_string(line_number));
        }
        return EventIndex(static_cast<uint32_t>(parsed));
    }

    static CommandType parse_command_type(const std::string& value, size_t line_number) {
        if (value == "NEW_ORDER") return CommandType::NEW_ORDER;
        if (value == "CANCEL_ORDER") return CommandType::CANCEL_ORDER;
        throw std::runtime_error("Invalid command type at log line " +
                                 std::to_string(line_number));
    }

    static RejectReason parse_reject_reason(const std::string& value, size_t line_number) {
        const RejectReason reasons[] = {
            RejectReason::DUPLICATE_ORDER_ID, RejectReason::INVALID_PRICE,
            RejectReason::INVALID_QUANTITY, RejectReason::POOL_EXHAUSTED,
            RejectReason::INDEX_EXHAUSTED, RejectReason::ORDER_NOT_FOUND,
            RejectReason::PRICE_OUT_OF_RANGE, RejectReason::OFF_TICK_PRICE,
            RejectReason::QUANTITY_LIMIT, RejectReason::INVALID_LOT_SIZE,
            RejectReason::INVALID_COMMAND_SEQUENCE
        };
        for (RejectReason reason : reasons) {
            if (value == to_string(reason)) return reason;
        }
        throw std::runtime_error("Invalid reject reason at log line " +
                                 std::to_string(line_number));
    }

public:
    static OrderBook replay_commands(const std::vector<Command>& commands,
                                     InstrumentConfig instrument_config = {},
                                     PriceLadderConfig price_config = {},
                                     size_t requested_capacity = 0) {
        const size_t capacity = requested_capacity != 0
            ? requested_capacity : (commands.empty() ? 1 : commands.size() * 2);
        OrderBook book(capacity, true, price_config, 2, instrument_config);
        for (const Command& command : commands) {
            (void)book.process(command);
        }
        return book;
    }

    // Existing CSV support is retained only as a diagnostic event round-trip.
    // Replay input is now an explicit Command sequence, never this event file.
    static void save_log(const std::vector<EngineEvent>& log, const std::string& filename) {
        std::ofstream file(filename);
        if (!file) throw std::runtime_error("Cannot open file: " + filename);
        char buffer[256];
        for (const EngineEvent& event : log) {
            event_to_buffer(event, buffer, sizeof(buffer));
            file << buffer << '\n';
        }
    }

    static std::vector<EngineEvent> load_log(const std::string& filename) {
        std::ifstream file(filename);
        if (!file) throw std::runtime_error("Cannot open file: " + filename);

        std::vector<EngineEvent> log;
        std::string line;
        size_t line_number = 0;
        while (std::getline(file, line)) {
            ++line_number;
            if (line.empty()) continue;

            std::stringstream stream(line);
            std::string segment;
            std::vector<std::string> parts;
            while (std::getline(stream, segment, ',')) parts.push_back(segment);
            if (parts.empty()) continue;

            if (parts[0] == "TRADE") {
                if (parts.size() != 7) {
                    throw std::runtime_error("Invalid TRADE field count at log line " +
                                             std::to_string(line_number));
                }
                log.emplace_back(std::in_place_type<TradeEvent>,
                    CommandSequence(parse_u64(parts[1], line_number, "command sequence")),
                    parse_event_index(parts[2], line_number),
                    OrderId(parse_u64(parts[3], line_number, "passive order id")),
                    OrderId(parse_u64(parts[4], line_number, "aggressive order id")),
                    Price(parse_i64(parts[5], line_number, "price")),
                    Quantity(parse_u64(parts[6], line_number, "quantity")));
            } else if (parts[0] == "ORDER_RESTED") {
                if (parts.size() != 9) {
                    throw std::runtime_error("Invalid ORDER_RESTED field count at log line " +
                                             std::to_string(line_number));
                }
                log.emplace_back(std::in_place_type<OrderRestedEvent>,
                    CommandSequence(parse_u64(parts[1], line_number, "command sequence")),
                    parse_event_index(parts[2], line_number),
                    OrderId(parse_u64(parts[3], line_number, "order id")),
                    parse_side(parts[4], line_number),
                    Price(parse_i64(parts[5], line_number, "price")),
                    Quantity(parse_u64(parts[6], line_number, "original quantity")),
                    Quantity(parse_u64(parts[7], line_number, "remaining quantity")),
                    PrioritySequence(parse_u64(parts[8], line_number, "priority sequence")));
            } else if (parts[0] == "ORDER_CANCELLED") {
                if (parts.size() != 8) {
                    throw std::runtime_error("Invalid ORDER_CANCELLED field count at log line " +
                                             std::to_string(line_number));
                }
                log.emplace_back(std::in_place_type<OrderCancelledEvent>,
                    CommandSequence(parse_u64(parts[1], line_number, "command sequence")),
                    parse_event_index(parts[2], line_number),
                    OrderId(parse_u64(parts[3], line_number, "order id")),
                    parse_side(parts[4], line_number),
                    Price(parse_i64(parts[5], line_number, "price")),
                    Quantity(parse_u64(parts[6], line_number, "cancelled quantity")),
                    PrioritySequence(parse_u64(parts[7], line_number, "priority sequence")));
            } else if (parts[0] == "ORDER_REJECTED") {
                if (parts.size() != 6) {
                    throw std::runtime_error("Invalid ORDER_REJECTED field count at log line " +
                                             std::to_string(line_number));
                }
                log.emplace_back(std::in_place_type<OrderRejectedEvent>,
                    CommandSequence(parse_u64(parts[1], line_number, "command sequence")),
                    parse_event_index(parts[2], line_number),
                    parse_command_type(parts[3], line_number),
                    OrderId(parse_u64(parts[4], line_number, "order id")),
                    parse_reject_reason(parts[5], line_number));
            } else {
                throw std::runtime_error("Unknown event type at log line " +
                                         std::to_string(line_number));
            }
        }
        return log;
    }
};

#endif
