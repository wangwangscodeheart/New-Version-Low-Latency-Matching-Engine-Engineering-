#ifndef PRICE_LEVEL_STORE_HPP
#define PRICE_LEVEL_STORE_HPP

#include "order.hpp"
#include "limit_level_pool.hpp"
#include <map>
#include <utility>

template <typename Compare>
class PriceLevelStore {
    using Map = std::map<int64_t, LimitLevel*, Compare>;
    Map levels_;
    LimitLevelPool* pool_;
public:
    using iterator = typename Map::iterator;
    using const_iterator = typename Map::const_iterator;
    explicit PriceLevelStore(LimitLevelPool& pool) noexcept : pool_(&pool) {}
    PriceLevelStore(const PriceLevelStore&) = delete;
    PriceLevelStore& operator=(const PriceLevelStore&) = delete;
    PriceLevelStore(PriceLevelStore&&) noexcept = default;
    PriceLevelStore& operator=(PriceLevelStore&&) noexcept = default;
    void rebind_pool(LimitLevelPool& pool) noexcept { pool_ = &pool; }
    bool empty() const noexcept { return levels_.empty(); }
    size_t size() const noexcept { return levels_.size(); }
    iterator begin() noexcept { return levels_.begin(); }
    const_iterator begin() const noexcept { return levels_.begin(); }
    iterator end() noexcept { return levels_.end(); }
    const_iterator end() const noexcept { return levels_.end(); }
    LimitLevel* find(int64_t price) noexcept {
        auto it = levels_.find(price);
        return it == levels_.end() ? nullptr : it->second;
    }
    const LimitLevel* find(int64_t price) const noexcept {
        auto it = levels_.find(price);
        return it == levels_.end() ? nullptr : it->second;
    }
    std::pair<LimitLevel*, bool> get_or_create(int64_t price) {
        auto hint = levels_.lower_bound(price);
        if (hint != levels_.end() && hint->first == price) {
            return {hint->second, false};
        }
        LimitLevel* level = pool_->acquire(Price(price));
        if (!level) return {nullptr, false};
        try {
            levels_.emplace_hint(hint, price, level);
            return {level, true};
        } catch (...) {
            pool_->release_owned_empty(level);
            throw;
        }
    }
    iterator erase(iterator it) {
        LimitLevel* level = it->second;
        auto next = levels_.erase(it);
        pool_->release_owned_empty(level);
        return next;
    }
    size_t erase(int64_t price) {
        auto it = levels_.find(price);
        if (it == levels_.end()) return 0;
        erase(it);
        return 1;
    }
};
#endif
