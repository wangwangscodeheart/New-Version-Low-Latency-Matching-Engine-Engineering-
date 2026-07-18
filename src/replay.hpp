#ifndef REPLAY_HPP
#define REPLAY_HPP

#include "orderbook.hpp"
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <stdexcept>

// ============================================================================
// REPLAY ENGINE - Determinism Verification
// ============================================================================

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

    static RejectReason parse_reject_reason(const std::string& value, size_t line_number) {
        const RejectReason reasons[] = {
            RejectReason::DUPLICATE_ORDER_ID, RejectReason::INVALID_PRICE,
            RejectReason::INVALID_QUANTITY, RejectReason::POOL_EXHAUSTED,
            RejectReason::INDEX_EXHAUSTED, RejectReason::ORDER_NOT_FOUND,
            RejectReason::PRICE_OUT_OF_RANGE, RejectReason::OFF_TICK_PRICE,
            RejectReason::QUANTITY_LIMIT, RejectReason::INVALID_LOT_SIZE
        };
        for (RejectReason reason : reasons) {
            if (value == to_string(reason)) return reason;
        }
        throw std::runtime_error("Invalid reject reason at log line " +
                                 std::to_string(line_number));
    }

public:
    // Replay from in-memory event log
    // Returns a reconstructed OrderBook state
    static OrderBook replay_from_log(const std::vector<Event>& log,
                                     InstrumentConfig instrument_config = {},
                                     PriceLadderConfig price_config = {}) {
        // Estimate capacity from log size to avoid reallocation
        const size_t capacity = log.empty() ? 1 : log.size() * 2;
        OrderBook book(capacity, true, price_config, 2, instrument_config);
        
        for (const auto& event : log) {
            std::visit([&book](auto&& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, NewOrderEvent>) {
                    // Replay NewOrder: Re-inject into book
                    book.process_new_order(e.order_id, e.side, e.price, e.quantity);
                }
                else if constexpr (std::is_same_v<T, CancelOrderEvent>) {
                    // Replay Cancel: Re-inject into book
                    book.process_cancel(e.order_id);
                }
                else if constexpr (std::is_same_v<T, RejectOrderEvent>) {
                    // Rejections are outcomes, but their logical time must be preserved.
                    book.advance_replay_time(e.timestamp);
                }
            }, event);
        }
        
        return book;
    }
    
    // Save event log to CSV file
    static void save_log(const std::vector<Event>& log, const std::string& filename) {
        std::ofstream file(filename);
        if (!file) {
            throw std::runtime_error("Cannot open file: " + filename);
        }
        
        char buffer[256];
        for (const auto& event : log) {
            // Use our zero-alloc to_buffer helper
            event_to_buffer(event, buffer, sizeof(buffer));
            file << buffer << "\n";
        }
    }
    
    // Load events from CSV file
    static std::vector<Event> load_log(const std::string& filename) {
        std::ifstream file(filename);
        if (!file) {
            throw std::runtime_error("Cannot open file: " + filename);
        }
        
        std::vector<Event> log;
        std::string line;
        size_t line_number = 0;
        
        while (std::getline(file, line)) {
            ++line_number;
            if (line.empty()) continue;
            
            // Minimalistic CSV parser (Fast)
            std::stringstream ss(line);
            std::string segment;
            std::vector<std::string> parts;
            
            while (std::getline(ss, segment, ',')) {
                parts.push_back(segment);
            }
            
            if (parts.empty()) continue;
            
            const std::string& type = parts[0];
            
            if (type == "NEW_ORDER") {
                if (parts.size() != 6) throw std::runtime_error("Invalid NEW_ORDER field count at log line " + std::to_string(line_number));
                // Format: NEW_ORDER,timestamp,id,side,price,qty
                Timestamp ts(parse_u64(parts[1], line_number, "timestamp"));
                OrderId id(parse_u64(parts[2], line_number, "order id"));
                Side side;
                if (parts[3] == "BUY") side = Side::BUY;
                else if (parts[3] == "SELL") side = Side::SELL;
                else throw std::runtime_error("Invalid side at log line " + std::to_string(line_number));
                Price price(parse_i64(parts[4], line_number, "price"));
                Quantity qty(parse_u64(parts[5], line_number, "quantity"));
                
                log.emplace_back(std::in_place_type<NewOrderEvent>, ts, id, side, price, qty);
            }
            else if (type == "CANCEL_ORDER") {
                if (parts.size() != 3) throw std::runtime_error("Invalid CANCEL_ORDER field count at log line " + std::to_string(line_number));
                // Format: CANCEL_ORDER,timestamp,id
                Timestamp ts(parse_u64(parts[1], line_number, "timestamp"));
                OrderId id(parse_u64(parts[2], line_number, "order id"));
                
                log.emplace_back(std::in_place_type<CancelOrderEvent>, ts, id);
            }
            else if (type == "TRADE") {
                if (parts.size() != 6) throw std::runtime_error("Invalid TRADE field count at log line " + std::to_string(line_number));
                log.emplace_back(std::in_place_type<TradeEvent>,
                    Timestamp(parse_u64(parts[1], line_number, "timestamp")),
                    OrderId(parse_u64(parts[2], line_number, "passive order id")),
                    OrderId(parse_u64(parts[3], line_number, "aggressive order id")),
                    Price(parse_i64(parts[4], line_number, "price")),
                    Quantity(parse_u64(parts[5], line_number, "quantity")));
            }
            else if (type == "REJECT_ORDER") {
                if (parts.size() != 4) throw std::runtime_error("Invalid REJECT_ORDER field count at log line " + std::to_string(line_number));
                log.emplace_back(std::in_place_type<RejectOrderEvent>,
                    Timestamp(parse_u64(parts[1], line_number, "timestamp")),
                    OrderId(parse_u64(parts[2], line_number, "order id")),
                    parse_reject_reason(parts[3], line_number));
            }
            else {
                throw std::runtime_error("Unknown event type at log line " + std::to_string(line_number));
            }
        }
        
        return log;
    }
};

#endif
