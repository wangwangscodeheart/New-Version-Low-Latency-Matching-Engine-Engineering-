#ifndef CAPACITY_METRICS_HPP
#define CAPACITY_METRICS_HPP

#include <cstddef>

struct CapacityMetrics {
    size_t active_orders = 0;
    size_t peak_active_orders = 0;
    size_t active_levels = 0;
    size_t peak_active_levels = 0;
    size_t index_load = 0;
    size_t index_capacity = 0;
    size_t order_capacity = 0;
    size_t level_capacity = 0;
    size_t event_size = 0;
    size_t event_capacity = 0;

    double index_load_factor() const noexcept {
        return index_capacity == 0 ? 0.0 :
            static_cast<double>(index_load) / static_cast<double>(index_capacity);
    }
};

#endif
