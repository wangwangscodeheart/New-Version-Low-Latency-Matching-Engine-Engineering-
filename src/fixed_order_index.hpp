#ifndef FIXED_ORDER_INDEX_HPP
#define FIXED_ORDER_INDEX_HPP

#include "order.hpp"
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

// Fixed-capacity open-addressing index for live orders.
// The table is allocated once and never grows during matching.
class FixedOrderIndex {
private:
    struct Slot {
        uint64_t key = 0;
        Order* value = nullptr;
        uint8_t state = 0; // 0 empty, 1 occupied
    };

    std::vector<Slot> slots_;
    size_t mask_ = 0;
    size_t size_ = 0;

    static size_t next_power_of_two(size_t value) {
        size_t result = 1;
        while (result < value) result <<= 1;
        return result;
    }

    static uint64_t hash_key(uint64_t key) {
        // Fibonacci multiplication is inexpensive and distributes sequential
        // order IDs well when the table keeps a low load factor.
        return key * 11400714819323198485ULL;
    }

public:
    explicit FixedOrderIndex(size_t capacity)
        : slots_(next_power_of_two(capacity * 2 + 1)),
          mask_(slots_.size() - 1) {}

    FixedOrderIndex(const FixedOrderIndex&) = delete;
    FixedOrderIndex& operator=(const FixedOrderIndex&) = delete;
    FixedOrderIndex(FixedOrderIndex&&) noexcept = default;
    FixedOrderIndex& operator=(FixedOrderIndex&&) noexcept = default;

    Order* find(uint64_t key) const {
        size_t index = static_cast<size_t>(hash_key(key)) & mask_;
        for (size_t probes = 0; probes < slots_.size(); ++probes) {
            const Slot& slot = slots_[index];
            if (slot.state == 0) return nullptr;
            if (slot.state == 1 && slot.key == key) return slot.value;
            index = (index + 1) & mask_;
        }
        return nullptr;
    }

    bool insert(uint64_t key, Order* value) {
        size_t index = static_cast<size_t>(hash_key(key)) & mask_;
        for (size_t probes = 0; probes < slots_.size(); ++probes) {
            Slot& slot = slots_[index];
            if (slot.state == 1 && slot.key == key) return false;
            if (slot.state == 0) {
                slots_[index] = Slot{key, value, 1};
                ++size_;
                return true;
            }
            index = (index + 1) & mask_;
        }
        return false;
    }

    bool erase(uint64_t key) {
        size_t index = static_cast<size_t>(hash_key(key)) & mask_;
        for (size_t probes = 0; probes < slots_.size(); ++probes) {
            Slot& slot = slots_[index];
            if (slot.state == 0) return false;
            if (slot.state == 1 && slot.key == key) {
                slot.value = nullptr;
                slot.state = 0;
                --size_;

                // Backward-shift deletion keeps probe chains intact without
                // accumulating tombstones during long order churn.
                size_t next = (index + 1) & mask_;
                while (slots_[next].state == 1) {
                    const uint64_t moved_key = slots_[next].key;
                    Order* moved_value = slots_[next].value;
                    slots_[next].state = 0;
                    slots_[next].value = nullptr;
                    --size_;
                    insert(moved_key, moved_value);
                    next = (next + 1) & mask_;
                }
                return true;
            }
            index = (index + 1) & mask_;
        }
        return false;
    }

    size_t size() const { return size_; }
    size_t capacity() const { return slots_.size(); }
};

#endif
