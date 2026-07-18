#ifndef EVENTS_HPP
#define EVENTS_HPP

#include "commands.hpp"
#include "types.hpp"
#include <cinttypes>
#include <cstdio>
#include <variant>

enum class EventType : uint8_t {
    TRADE = 0,
    ORDER_RESTED = 1,
    ORDER_CANCELLED = 2,
    ORDER_REJECTED = 3
};

enum class RejectReason : uint8_t {
    DUPLICATE_ORDER_ID = 0,
    INVALID_PRICE = 1,
    INVALID_QUANTITY = 2,
    POOL_EXHAUSTED = 3,
    INDEX_EXHAUSTED = 4,
    ORDER_NOT_FOUND = 5,
    PRICE_OUT_OF_RANGE = 6,
    OFF_TICK_PRICE = 7,
    QUANTITY_LIMIT = 8,
    INVALID_LOT_SIZE = 9
};

inline const char* to_string(RejectReason reason) noexcept {
    switch (reason) {
        case RejectReason::DUPLICATE_ORDER_ID: return "DUPLICATE_ORDER_ID";
        case RejectReason::INVALID_PRICE: return "INVALID_PRICE";
        case RejectReason::INVALID_QUANTITY: return "INVALID_QUANTITY";
        case RejectReason::POOL_EXHAUSTED: return "POOL_EXHAUSTED";
        case RejectReason::INDEX_EXHAUSTED: return "INDEX_EXHAUSTED";
        case RejectReason::ORDER_NOT_FOUND: return "ORDER_NOT_FOUND";
        case RejectReason::PRICE_OUT_OF_RANGE: return "PRICE_OUT_OF_RANGE";
        case RejectReason::OFF_TICK_PRICE: return "OFF_TICK_PRICE";
        case RejectReason::QUANTITY_LIMIT: return "QUANTITY_LIMIT";
        case RejectReason::INVALID_LOT_SIZE: return "INVALID_LOT_SIZE";
    }
    return "UNKNOWN";
}

struct TradeEvent {
    EventType type;
    Timestamp timestamp;
    OrderId passive_order_id;
    OrderId aggressive_order_id;
    Price price;
    Quantity quantity;

    TradeEvent(Timestamp ts, OrderId passive, OrderId aggressive, Price trade_price,
               Quantity trade_quantity)
        : type(EventType::TRADE), timestamp(ts), passive_order_id(passive),
          aggressive_order_id(aggressive), price(trade_price), quantity(trade_quantity) {}

    void to_buffer(char* buffer, size_t size) const {
        std::snprintf(buffer, size,
                      "TRADE,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRId64 ",%" PRIu64,
                      timestamp.get(), passive_order_id.get(), aggressive_order_id.get(),
                      price.get(), quantity.get());
    }
};

struct OrderRestedEvent {
    EventType type;
    Timestamp timestamp;
    OrderId order_id;
    Side side;
    Price price;
    Quantity original_quantity;
    Quantity remaining_quantity;

    OrderRestedEvent(Timestamp ts, OrderId id, Side order_side, Price order_price,
                     Quantity original, Quantity remaining)
        : type(EventType::ORDER_RESTED), timestamp(ts), order_id(id), side(order_side),
          price(order_price), original_quantity(original), remaining_quantity(remaining) {}

    void to_buffer(char* buffer, size_t size) const {
        std::snprintf(buffer, size,
                      "ORDER_RESTED,%" PRIu64 ",%" PRIu64 ",%s,%" PRId64 ",%" PRIu64 ",%" PRIu64,
                      timestamp.get(), order_id.get(), to_string(side), price.get(),
                      original_quantity.get(), remaining_quantity.get());
    }
};

struct OrderCancelledEvent {
    EventType type;
    Timestamp timestamp;
    OrderId order_id;
    Side side;
    Price price;
    Quantity cancelled_quantity;

    OrderCancelledEvent(Timestamp ts, OrderId id, Side order_side, Price order_price,
                        Quantity quantity)
        : type(EventType::ORDER_CANCELLED), timestamp(ts), order_id(id), side(order_side),
          price(order_price), cancelled_quantity(quantity) {}

    void to_buffer(char* buffer, size_t size) const {
        std::snprintf(buffer, size,
                      "ORDER_CANCELLED,%" PRIu64 ",%" PRIu64 ",%s,%" PRId64 ",%" PRIu64,
                      timestamp.get(), order_id.get(), to_string(side), price.get(),
                      cancelled_quantity.get());
    }
};

struct OrderRejectedEvent {
    EventType type;
    Timestamp timestamp;
    CommandType command_type;
    OrderId order_id;
    RejectReason reason;

    OrderRejectedEvent(Timestamp ts, CommandType rejected_command_type, OrderId id,
                       RejectReason reject_reason)
        : type(EventType::ORDER_REJECTED), timestamp(ts),
          command_type(rejected_command_type), order_id(id), reason(reject_reason) {}

    void to_buffer(char* buffer, size_t size) const {
        const char* command_name = command_type == CommandType::NEW_ORDER
            ? "NEW_ORDER" : "CANCEL_ORDER";
        std::snprintf(buffer, size, "ORDER_REJECTED,%" PRIu64 ",%s,%" PRIu64 ",%s",
                      timestamp.get(), command_name, order_id.get(), to_string(reason));
    }
};

using EngineEvent = std::variant<TradeEvent, OrderRestedEvent,
                                 OrderCancelledEvent, OrderRejectedEvent>;

inline EventType get_event_type(const EngineEvent& event) noexcept {
    return std::visit([](const auto& value) { return value.type; }, event);
}

inline Timestamp get_timestamp(const EngineEvent& event) noexcept {
    return std::visit([](const auto& value) { return value.timestamp; }, event);
}

inline void event_to_buffer(const EngineEvent& event, char* buffer, size_t size) {
    std::visit([buffer, size](const auto& value) { value.to_buffer(buffer, size); }, event);
}

#endif
