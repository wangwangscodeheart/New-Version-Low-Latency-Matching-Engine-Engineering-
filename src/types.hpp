#ifndef TYPES_HPP
#define TYPES_HPP

#include <cstdint>
#include <string>

// ============================================================================
// STRONG TYPES - Compile-time Type Safety
// ============================================================================

template<typename T, typename Tag>
struct StrongType {
    T value;
    
    explicit constexpr StrongType(T v) : value(v) {}
    
    constexpr T get() const { return value; }
    
    constexpr bool operator<(const StrongType& other) const { 
        return value < other.value; 
    }
    
    constexpr bool operator<=(const StrongType& other) const { 
        return value <= other.value; 
    }
    
    constexpr bool operator>(const StrongType& other) const { 
        return value > other.value; 
    }
    
    constexpr bool operator>=(const StrongType& other) const { 
        return value >= other.value; 
    }
    
    constexpr bool operator==(const StrongType& other) const { 
        return value == other.value; 
    }
    
    constexpr bool operator!=(const StrongType& other) const { 
        return value != other.value; 
    }
};

// Type tags for disambiguation
struct OrderIdTag {};
struct PriceTag {};
struct QuantityTag {};
struct TimestampTag {};
struct CommandSequenceTag {};
struct EventIndexTag {};
struct PrioritySequenceTag {};

// Strong type aliases
using OrderId = StrongType<uint64_t, OrderIdTag>;
using Price = StrongType<int64_t, PriceTag>;
using Quantity = StrongType<uint64_t, QuantityTag>;
using Timestamp = StrongType<uint64_t, TimestampTag>;
using CommandSequence = StrongType<uint64_t, CommandSequenceTag>;
using EventIndex = StrongType<uint32_t, EventIndexTag>;
using PrioritySequence = StrongType<uint64_t, PrioritySequenceTag>;

// Price scaling factor
constexpr int64_t PRICE_SCALE = 10000;

// Helper function for price conversion
constexpr Price from_double(double price) {
    return Price(static_cast<int64_t>(price * PRICE_SCALE + (price >= 0.0 ? 0.5 : -0.5)));
}

constexpr double to_double(Price price) {
    return static_cast<double>(price.get()) / PRICE_SCALE;
}

// Side enum
enum class Side : uint8_t { 
    BUY = 0, 
    SELL = 1
};

// Helper function
inline const char* to_string(Side side) {
    return side == Side::BUY ? "BUY" : "SELL";
}

#endif
