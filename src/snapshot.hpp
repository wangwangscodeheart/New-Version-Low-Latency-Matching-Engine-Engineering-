#ifndef SNAPSHOT_HPP
#define SNAPSHOT_HPP

#include "instrument_config.hpp"
#include "price_ladder.hpp"
#include "types.hpp"
#include <cstdint>
#include <vector>

constexpr uint32_t CURRENT_SNAPSHOT_VERSION = 1;

struct SnapshotOrder {
    OrderId order_id;
    Side side;
    Price price;
    Quantity original_quantity;
    Quantity remaining_quantity;
    PrioritySequence priority_sequence;
};

struct OrderBookSnapshot {
    uint32_t snapshot_version = CURRENT_SNAPSHOT_VERSION;
    CommandSequence last_applied_command_sequence{0};
    InstrumentConfig instrument_config{};
    PriceLadderConfig price_ladder_config{};
    std::vector<SnapshotOrder> active_orders;
};

enum class SnapshotRestoreError : uint8_t {
    NONE = 0,
    UNSUPPORTED_VERSION = 1,
    INVALID_INSTRUMENT_CONFIG = 2,
    INVALID_PRICE_LADDER_CONFIG = 3,
    CAPACITY_EXCEEDED = 4,
    DUPLICATE_ORDER_ID = 5,
    INVALID_SIDE = 6,
    INVALID_PRICE = 7,
    INVALID_QUANTITY = 8,
    INVALID_PRIORITY_SEQUENCE = 9,
    DUPLICATE_PRIORITY_SEQUENCE = 10,
    CROSSED_BOOK = 11,
    LEVEL_VOLUME_OVERFLOW = 12,
    ALLOCATION_FAILURE = 13,
    INTERNAL_RESTORE_FAILURE = 14
};

inline const char* to_string(SnapshotRestoreError error) noexcept {
    switch (error) {
        case SnapshotRestoreError::NONE: return "NONE";
        case SnapshotRestoreError::UNSUPPORTED_VERSION: return "UNSUPPORTED_VERSION";
        case SnapshotRestoreError::INVALID_INSTRUMENT_CONFIG: return "INVALID_INSTRUMENT_CONFIG";
        case SnapshotRestoreError::INVALID_PRICE_LADDER_CONFIG: return "INVALID_PRICE_LADDER_CONFIG";
        case SnapshotRestoreError::CAPACITY_EXCEEDED: return "CAPACITY_EXCEEDED";
        case SnapshotRestoreError::DUPLICATE_ORDER_ID: return "DUPLICATE_ORDER_ID";
        case SnapshotRestoreError::INVALID_SIDE: return "INVALID_SIDE";
        case SnapshotRestoreError::INVALID_PRICE: return "INVALID_PRICE";
        case SnapshotRestoreError::INVALID_QUANTITY: return "INVALID_QUANTITY";
        case SnapshotRestoreError::INVALID_PRIORITY_SEQUENCE: return "INVALID_PRIORITY_SEQUENCE";
        case SnapshotRestoreError::DUPLICATE_PRIORITY_SEQUENCE: return "DUPLICATE_PRIORITY_SEQUENCE";
        case SnapshotRestoreError::CROSSED_BOOK: return "CROSSED_BOOK";
        case SnapshotRestoreError::LEVEL_VOLUME_OVERFLOW: return "LEVEL_VOLUME_OVERFLOW";
        case SnapshotRestoreError::ALLOCATION_FAILURE: return "ALLOCATION_FAILURE";
        case SnapshotRestoreError::INTERNAL_RESTORE_FAILURE: return "INTERNAL_RESTORE_FAILURE";
    }
    return "UNKNOWN";
}

#endif
