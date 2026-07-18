#ifndef LIMIT_LEVEL_POOL_HPP
#define LIMIT_LEVEL_POOL_HPP

#include "order.hpp"
#include <cstddef>
#include <memory>
#include <vector>
#include <cstdint>
#include <exception>

// Fixed-capacity storage for LimitLevel objects. This is intentionally kept
// independent from PriceLevelStore until lifecycle tests are complete.
class LimitLevelPool {
    std::vector<LimitLevel> slots_;
    std::vector<size_t> free_slots_;
    std::vector<uint8_t> in_use_;

    void release_slot(size_t index) noexcept {
        in_use_[index] = 0;
        slots_[index] = LimitLevel(Price(0));
        free_slots_.push_back(index);
    }
public:
    explicit LimitLevelPool(size_t capacity)
        : slots_(capacity), free_slots_(), in_use_(capacity, 0) {
        free_slots_.reserve(capacity);
        for (size_t i = capacity; i > 0; --i) free_slots_.push_back(i - 1);
    }

    LimitLevel* acquire(Price price) noexcept {
        if (free_slots_.empty()) return nullptr;
        const size_t index = free_slots_.back();
        free_slots_.pop_back();
        in_use_[index] = 1;
        LimitLevel* level = &slots_[index];
        *level = LimitLevel(price);
        return level;
    }

    void release(LimitLevel* level) noexcept {
        if (!level) return;
        const auto base = reinterpret_cast<uintptr_t>(slots_.data());
        const auto addr = reinterpret_cast<uintptr_t>(level);
        if (addr < base || addr >= base + slots_.size() * sizeof(LimitLevel) ||
            ((addr - base) % sizeof(LimitLevel)) != 0) return;
        const size_t index = static_cast<size_t>((addr - base) / sizeof(LimitLevel));
        if (index >= slots_.size()) return;
        if (!in_use_[index] || !level->empty()) return;
        release_slot(index);
    }

    // Hot-path release for a level owned by PriceLevelStore. Its ownership
    // contract guarantees that the pointer originates from this pool.
    void release_owned_empty(LimitLevel* level) noexcept {
        const size_t index = static_cast<size_t>(level - slots_.data());
        if (index >= slots_.size() || !in_use_[index] || !level->empty()) {
            std::terminate();
        }
        release_slot(index);
    }

    size_t capacity() const noexcept { return slots_.size(); }
    size_t available() const noexcept { return free_slots_.size(); }
};

#endif
