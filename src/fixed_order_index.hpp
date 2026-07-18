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
    enum class InsertStatus : uint8_t {
        AVAILABLE = 0,
        DUPLICATE = 1,
        FULL = 2
    };

    struct InsertReservation {
        size_t slot = 0;
        InsertStatus status = InsertStatus::FULL;
    };

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
            if (slot.key == 0) return nullptr;
            if (slot.key == key) return slot.value;
            index = (index + 1) & mask_;
        }
        return nullptr;
    }

    InsertReservation prepare_insert(uint64_t key) const noexcept {
        size_t index = static_cast<size_t>(hash_key(key)) & mask_;
        for (size_t probes = 0; probes < slots_.size(); ++probes) {
            const Slot& slot = slots_[index];
            if (slot.key == key) {
                return InsertReservation{index, InsertStatus::DUPLICATE};
            }
            if (slot.key == 0) {
                return InsertReservation{index, InsertStatus::AVAILABLE};
            }
            index = (index + 1) & mask_;
        }
        return InsertReservation{0, InsertStatus::FULL};
    }

    // The engine is a single-writer design: no index mutation may occur
    // between prepare_insert() and commit_insert(). This avoids probing the
    // same hash chain twice while preserving duplicate-first rejection.
    bool commit_insert(const InsertReservation& reservation,
                       uint64_t key, Order* value) noexcept {
        if (reservation.status != InsertStatus::AVAILABLE ||
            reservation.slot >= slots_.size() ||
            slots_[reservation.slot].key != 0) {
            return false;
        }
        slots_[reservation.slot] = Slot{key, value};
        ++size_;
        return true;
    }

    bool insert(uint64_t key, Order* value) noexcept {
        const InsertReservation reservation = prepare_insert(key);
        return commit_insert(reservation, key, value);
    }

    bool erase(uint64_t key) {
        size_t index = static_cast<size_t>(hash_key(key)) & mask_;
        for (size_t probes = 0; probes < slots_.size(); ++probes) {
            Slot& slot = slots_[index];
            if (slot.key == 0) return false;
            if (slot.key == key) {
                slot.key = 0;
                slot.value = nullptr;
                --size_;

                // Backward-shift deletion keeps probe chains intact without
                // accumulating tombstones during long order churn.
                size_t next = (index + 1) & mask_;
                while (slots_[next].key != 0) {
                    const uint64_t moved_key = slots_[next].key;
                    Order* moved_value = slots_[next].value;
                    slots_[next].key = 0;
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
    static constexpr size_t slot_size_bytes() noexcept { return sizeof(Slot); }
};

#endif
