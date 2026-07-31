#ifndef SYSTEM_EVENTS_HPP
#define SYSTEM_EVENTS_HPP

#include "commands.hpp"
#include "events.hpp"
#include "symbol.hpp"
#include "types.hpp"
#include <cstdint>
#include <string>
#include <utility>
#include <variant>

// System events live outside the matching core. They add routing and wall/event
// time context while preserving the existing deterministic Command/EngineEvent
// model used by OrderBook.
struct MarketDataEvent {
    Price price;
    Quantity volume;
    Price bid;
    Price ask;
};

struct OrderEvent {
    Command command;
};

enum class CommandOutcome : uint8_t {
    APPLIED = 0,
    BUSINESS_REJECTED = 1,
    SEQUENCE_REJECTED = 2,
    UNKNOWN_SYMBOL = 3
};

struct ProcessingLatencyEvent {
    uint64_t nanoseconds;
    CommandOutcome outcome;
    CommandSequence command_sequence;
    OrderId order_id;
};

enum class DataQualitySeverity : uint8_t { WARNING = 0, ERROR = 1 };
enum class DataQualityCode : uint8_t {
    MISSING_FIELD = 0,
    PARSE_ERROR = 1,
    TIMESTAMP_BACKWARDS = 2,
    INVALID_VALUE = 3,
    CROSSED_MARKET = 4,
    PRICE_JUMP = 5,
    STALE_SYMBOL = 6
};

struct DataQualityEvent {
    DataQualitySeverity severity;
    DataQualityCode code;
    std::string message;
};

using SystemEventPayload =
    std::variant<MarketDataEvent, OrderEvent, EngineEvent, ProcessingLatencyEvent,
                 DataQualityEvent>;

enum class SystemEventType : uint8_t {
    MARKET_DATA = 0,
    ORDER = 1,
    TRADE = 2,
    ORDER_RESTED = 3,
    CANCEL = 4,
    ORDER_REJECTED = 5,
    PROCESSING_LATENCY = 6,
    DATA_QUALITY = 7
};

inline SystemEventType to_system_event_type(const EngineEvent& event) noexcept {
    switch (get_event_type(event)) {
        case EventType::TRADE: return SystemEventType::TRADE;
        case EventType::ORDER_RESTED: return SystemEventType::ORDER_RESTED;
        case EventType::ORDER_CANCELLED: return SystemEventType::CANCEL;
        case EventType::ORDER_REJECTED: return SystemEventType::ORDER_REJECTED;
    }
    return SystemEventType::ORDER_REJECTED;
}

struct SystemEvent {
    Timestamp timestamp;
    SystemEventType event_type;
    Symbol symbol;
    SystemEventPayload payload;

    static SystemEvent market_data(Timestamp timestamp, Symbol symbol,
                                   MarketDataEvent event) {
        return SystemEvent{timestamp, SystemEventType::MARKET_DATA,
                           std::move(symbol), std::move(event)};
    }

    static SystemEvent order(Timestamp timestamp, Symbol symbol, Command command) {
        return SystemEvent{timestamp, SystemEventType::ORDER, std::move(symbol),
                           OrderEvent{std::move(command)}};
    }

    static SystemEvent engine(Timestamp timestamp, Symbol symbol,
                              EngineEvent event) {
        const SystemEventType type = to_system_event_type(event);
        return SystemEvent{timestamp, type, std::move(symbol), std::move(event)};
    }

    static SystemEvent processing_latency(Timestamp timestamp, Symbol symbol,
                                          ProcessingLatencyEvent event) {
        return SystemEvent{timestamp, SystemEventType::PROCESSING_LATENCY,
                           std::move(symbol), event};
    }

    static SystemEvent data_quality(Timestamp timestamp, Symbol symbol,
                                    DataQualityEvent event) {
        return SystemEvent{timestamp, SystemEventType::DATA_QUALITY,
                           std::move(symbol), std::move(event)};
    }
};

#endif
