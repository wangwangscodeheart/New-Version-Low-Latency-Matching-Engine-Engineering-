#ifndef EVENTS_HPP
#define EVENTS_HPP

#include "types.hpp"
#include <variant>
#include <array>
#include <cinttypes>

// ============================================================================
// EVENT SYSTEM
// ============================================================================

enum class EventType : uint8_t {
    NEW_ORDER = 0,
    CANCEL_ORDER = 1,
    TRADE = 2,
    SNAPSHOT = 3,
    REJECT_ORDER = 4
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

inline const char* to_string(RejectReason reason) {
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

// ============================================================================
// EVENT STRUCTS - POD types
// ============================================================================

struct NewOrderEvent {
    EventType type;
    Timestamp timestamp;
    OrderId order_id;
    Side side;
    Price price;
    Quantity quantity;
    
    NewOrderEvent(Timestamp ts, OrderId id, Side s, Price p, Quantity q)
        : type(EventType::NEW_ORDER), timestamp(ts), 
          order_id(id), side(s), price(p), quantity(q) {}
    
    // Fast string formatting (avoid std::stringstream)
    void to_buffer(char* buffer, size_t size) const {
        snprintf(buffer, size, "NEW_ORDER,%" PRIu64 ",%" PRIu64 ",%s,%" PRId64 ",%" PRIu64,
                timestamp.get(), order_id.get(), to_string(side),
                price.get(), quantity.get());
    }
};

struct CancelOrderEvent {
    EventType type;
    Timestamp timestamp;
    OrderId order_id;
    
    CancelOrderEvent(Timestamp ts, OrderId id)
        : type(EventType::CANCEL_ORDER), timestamp(ts), order_id(id) {}
    
    void to_buffer(char* buffer, size_t size) const {
        snprintf(buffer, size, "CANCEL_ORDER,%" PRIu64 ",%" PRIu64,
                timestamp.get(), order_id.get());
    }
};

struct TradeEvent {
    EventType type;
    Timestamp timestamp;
    OrderId passive_order_id;
    OrderId aggressive_order_id;
    Price price;
    Quantity quantity;
    
    TradeEvent(Timestamp ts, OrderId passive, OrderId aggressive, 
               Price p, Quantity q)
        : type(EventType::TRADE), timestamp(ts), 
          passive_order_id(passive), aggressive_order_id(aggressive), 
          price(p), quantity(q) {}
    
    void to_buffer(char* buffer, size_t size) const {
        snprintf(buffer, size, "TRADE,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRId64 ",%" PRIu64,
                timestamp.get(), passive_order_id.get(), 
                aggressive_order_id.get(), price.get(), quantity.get());
    }
};

struct RejectOrderEvent {
    EventType type;
    Timestamp timestamp;
    OrderId order_id;
    RejectReason reason;

    RejectOrderEvent(Timestamp ts, OrderId id, RejectReason r)
        : type(EventType::REJECT_ORDER), timestamp(ts), order_id(id), reason(r) {}

    void to_buffer(char* buffer, size_t size) const {
        snprintf(buffer, size, "REJECT_ORDER,%llu,%llu,%s",
                 static_cast<unsigned long long>(timestamp.get()),
                 static_cast<unsigned long long>(order_id.get()),
                 to_string(reason));
    }
};

// ============================================================================
// EVENT VARIANT - Type-safe union without virtual functions
// ============================================================================

using Event = std::variant<NewOrderEvent, CancelOrderEvent, TradeEvent, RejectOrderEvent>;

// Helper for getting event type
inline EventType get_event_type(const Event& event) {
    return std::visit([](const auto& e) { return e.type; }, event);
}

// Helper for getting timestamp
inline Timestamp get_timestamp(const Event& event) {
    return std::visit([](const auto& e) { return e.timestamp; }, event);
}

// Helper for string conversion (for logging/debugging only)
inline void event_to_buffer(const Event& event, char* buffer, size_t size) {
    std::visit([buffer, size](const auto& e) { 
        e.to_buffer(buffer, size); 
    }, event);
}

#endif
