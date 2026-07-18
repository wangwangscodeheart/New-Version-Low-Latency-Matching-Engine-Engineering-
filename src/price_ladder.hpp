#ifndef PRICE_LADDER_HPP
#define PRICE_LADDER_HPP

#include "order.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>
#include <stdexcept>

// Hybrid price index: dense O(1) lookup inside a configured range, with the
// caller falling back to std::map for prices outside that range.
struct PriceLadderConfig {
    int64_t min_price = 900000;
    int64_t max_price = 2100000;
    int64_t tick = 100; // PRICE_SCALE=10000 -> 0.01
};

class PriceLadder {
private:
    int64_t min_price_;
    int64_t max_price_;
    int64_t tick_;
    std::vector<LimitLevel*> levels_;

    static size_t level_count(const PriceLadderConfig& config) {
        if (config.tick <= 0 || config.max_price < config.min_price ||
            ((config.max_price - config.min_price) % config.tick) != 0) {
            throw std::invalid_argument("Invalid price ladder configuration");
        }
        return static_cast<size_t>((config.max_price - config.min_price) / config.tick + 1);
    }

public:
    explicit PriceLadder(const PriceLadderConfig& config = {})
        : min_price_(config.min_price), max_price_(config.max_price), tick_(config.tick),
          levels_(level_count(config), nullptr) {}

    LimitLevel* find(int64_t price) const {
        if (price < min_price_ || price > max_price_) return nullptr;
        const int64_t offset = price - min_price_;
        if (offset % tick_ != 0) return nullptr;
        return levels_[static_cast<size_t>(offset / tick_)];
    }

    void set(int64_t price, LimitLevel* level) {
        if (price < min_price_ || price > max_price_) return;
        const int64_t offset = price - min_price_;
        if (offset % tick_ == 0) levels_[static_cast<size_t>(offset / tick_)] = level;
    }

    void clear(int64_t price) { set(price, nullptr); }
    size_t size() const { return levels_.size(); }
    bool contains_price(int64_t price) const noexcept {
        if (price < min_price_ || price > max_price_) return false;
        return (price - min_price_) % tick_ == 0;
    }
    size_t active_count() const noexcept {
        size_t count = 0;
        for (const LimitLevel* level : levels_) {
            if (level) ++count;
        }
        return count;
    }
    PriceLadderConfig config() const noexcept {
        return PriceLadderConfig{min_price_, max_price_, tick_};
    }
};

#endif
