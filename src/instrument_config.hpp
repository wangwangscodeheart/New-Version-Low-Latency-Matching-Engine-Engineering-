#ifndef INSTRUMENT_CONFIG_HPP
#define INSTRUMENT_CONFIG_HPP

#include "types.hpp"
#include <cstdint>
#include <limits>
#include <stdexcept>

// Per-instrument trading rules. Prices and quantities stay integer-only on the
// engine boundary; adapters must convert external decimal formats beforehand.
struct InstrumentConfig {
    int64_t tick_size = 100; // PRICE_SCALE=10000 -> 0.01
    uint64_t lot_size = 1;
    int64_t min_price = 1;
    int64_t max_price = std::numeric_limits<int64_t>::max();
    uint64_t max_order_quantity = std::numeric_limits<uint64_t>::max();

    void validate() const {
        if (tick_size <= 0 || lot_size == 0 || min_price <= 0 ||
            max_price < min_price || max_order_quantity == 0) {
            throw std::invalid_argument("Invalid instrument configuration");
        }
    }

    bool price_in_range(int64_t value) const noexcept {
        return value >= min_price && value <= max_price;
    }

    bool price_on_tick(int64_t value) const noexcept {
        return value % tick_size == 0;
    }

    bool quantity_in_range(uint64_t value) const noexcept {
        return value != 0 && value <= max_order_quantity;
    }

    bool quantity_on_lot(uint64_t value) const noexcept {
        return value % lot_size == 0;
    }
};

#endif
