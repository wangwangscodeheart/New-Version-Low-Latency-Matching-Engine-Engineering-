#ifndef ORDERBOOK_HPP
#define ORDERBOOK_HPP

#include "types.hpp"
#include "order.hpp"
#include "fixed_order_index.hpp"
#include "price_ladder.hpp"
#include "price_level_store.hpp"
#include "instrument_config.hpp"
#include "events.hpp"
#include <map>
#include <unordered_map>
#include <vector>
#include <optional>
#include <iostream>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>

// ============================================================================
// ORDER BOOK - HFT Optimized Matching Engine
// ============================================================================

class OrderBook {
private:
    // ------------------------------------------------------------------------
    // Memory Management (Hot Path)
    // ------------------------------------------------------------------------
    ObjectPool<Order> order_pool_;
    LimitLevelPool level_pool_;

    // ------------------------------------------------------------------------
    // Data Structures
    // ------------------------------------------------------------------------
    // Bids: highest first (Greater), Asks: lowest first (Less)
    // Key is int64_t (underlying type of Price) to avoid overhead
    PriceLevelStore<std::greater<int64_t>> bids_;
    PriceLevelStore<std::less<int64_t>> asks_;

    // Fast O(1) lookup by Order ID
    FixedOrderIndex order_index_;
    PriceLadder price_ladder_;
    
    // Event Log: Stores objects by value (contiguous memory)
    std::vector<Event> event_log_;
    
    Timestamp current_time_;
    bool record_events_;
    InstrumentConfig instrument_config_;
    LimitLevel* cached_bid_level_ = nullptr;
    LimitLevel* cached_ask_level_ = nullptr;
    int64_t cached_bid_price_ = 0;
    int64_t cached_ask_price_ = 0;

public:
    // Pre-allocate memory to avoid runtime allocation
    explicit OrderBook(size_t capacity = 1000000, bool record_events = true,
                         PriceLadderConfig price_config = {},
                         size_t event_reserve_multiplier = 2,
                         InstrumentConfig instrument_config = {})
          : order_pool_(capacity), level_pool_(capacity), bids_(level_pool_), asks_(level_pool_),
            order_index_(capacity), price_ladder_(price_config),
            current_time_(Timestamp(0)), record_events_(record_events),
            instrument_config_(instrument_config) {
          instrument_config_.validate();
          if (record_events_) {
              // A resting order normally emits one event, while an aggressive
              // order can also emit one or more trade events. The multiplier is
              // configurable so replay capture and low-memory backtests can
              // choose different reserve budgets. Guard multiplication overflow.
              const size_t max_size = std::numeric_limits<size_t>::max();
              const size_t reserve_events =
                  (event_reserve_multiplier != 0 &&
                   capacity <= max_size / event_reserve_multiplier)
                      ? capacity * event_reserve_multiplier : capacity;
              event_log_.reserve(reserve_events);
          }
    }

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    OrderBook(OrderBook&& other) noexcept
        : order_pool_(std::move(other.order_pool_)),
          level_pool_(std::move(other.level_pool_)),
          bids_(std::move(other.bids_)),
          asks_(std::move(other.asks_)),
          order_index_(std::move(other.order_index_)),
          price_ladder_(std::move(other.price_ladder_)),
          event_log_(std::move(other.event_log_)),
          current_time_(other.current_time_),
          record_events_(other.record_events_),
          instrument_config_(other.instrument_config_),
          cached_bid_level_(other.cached_bid_level_),
          cached_ask_level_(other.cached_ask_level_),
          cached_bid_price_(other.cached_bid_price_),
          cached_ask_price_(other.cached_ask_price_) {
        bids_.rebind_pool(level_pool_);
        asks_.rebind_pool(level_pool_);
        other.cached_bid_level_ = nullptr;
        other.cached_ask_level_ = nullptr;
    }

    OrderBook& operator=(OrderBook&&) = delete;

    // ========================================================================
    // PROCESS: NEW ORDER
    // ========================================================================
    void process_new_order(OrderId id, Side side, Price price, Quantity qty) {
        current_time_ = Timestamp(current_time_.get() + 1);

        if (id.get() == 0) {
            if (record_events_) event_log_.emplace_back(std::in_place_type<RejectOrderEvent>,
                                    current_time_, id, RejectReason::DUPLICATE_ORDER_ID);
            return;
        }
        const FixedOrderIndex::InsertReservation index_reservation =
            order_index_.prepare_insert(id.get());
        if (index_reservation.status == FixedOrderIndex::InsertStatus::DUPLICATE) {
            if (record_events_) event_log_.emplace_back(std::in_place_type<RejectOrderEvent>,
                                    current_time_, id, RejectReason::DUPLICATE_ORDER_ID);
            return;
        }
        if (price.get() <= 0) {
            if (record_events_) event_log_.emplace_back(std::in_place_type<RejectOrderEvent>,
                                    current_time_, id, RejectReason::INVALID_PRICE);
            return;
        }
        if (!instrument_config_.price_in_range(price.get())) {
            if (record_events_) event_log_.emplace_back(std::in_place_type<RejectOrderEvent>,
                                    current_time_, id, RejectReason::PRICE_OUT_OF_RANGE);
            return;
        }
        if (!instrument_config_.price_on_tick(price.get())) {
            if (record_events_) event_log_.emplace_back(std::in_place_type<RejectOrderEvent>,
                                    current_time_, id, RejectReason::OFF_TICK_PRICE);
            return;
        }
        if (qty.get() == 0) {
            if (record_events_) event_log_.emplace_back(std::in_place_type<RejectOrderEvent>,
                                    current_time_, id, RejectReason::INVALID_QUANTITY);
            return;
        }
        if (!instrument_config_.quantity_in_range(qty.get())) {
            if (record_events_) event_log_.emplace_back(std::in_place_type<RejectOrderEvent>,
                                    current_time_, id, RejectReason::QUANTITY_LIMIT);
            return;
        }
        if (!instrument_config_.quantity_on_lot(qty.get())) {
            if (record_events_) event_log_.emplace_back(std::in_place_type<RejectOrderEvent>,
                                    current_time_, id, RejectReason::INVALID_LOT_SIZE);
            return;
        }

        // Reserve the order object before acknowledging the command.
        Order* order = order_pool_.allocate();
        if (!order) {
            if (record_events_) event_log_.emplace_back(std::in_place_type<RejectOrderEvent>,
                                    current_time_, id, RejectReason::POOL_EXHAUSTED);
            std::cerr << "CRITICAL: Order Pool Exhausted!\n";
            return;
        }

        order->activate(id, current_time_, side, price, qty);

          // Index before acknowledging the command. A failed index insertion must
          // never leave a NEW_ORDER event in the replay stream.
          if (index_reservation.status != FixedOrderIndex::InsertStatus::AVAILABLE ||
              !order_index_.commit_insert(index_reservation, id.get(), order)) {
              order_pool_.deallocate(order);
              if (record_events_) event_log_.emplace_back(std::in_place_type<RejectOrderEvent>,
                                    current_time_, id, RejectReason::INDEX_EXHAUSTED);
              return;
          }

          // Only accepted commands are logged as NEW_ORDER.
          if (record_events_) event_log_.emplace_back(std::in_place_type<NewOrderEvent>,
                                current_time_, id, side, price, qty);

          // 3. Match logic

        // 4. Match logic
        if (side == Side::BUY) {
            match_order_buy(order);
        } else {
            match_order_sell(order);
        }

        // 5. Add remaining to book
        if (!order->is_filled()) {
            add_to_book(order);
        } else {
            order_index_.erase(id.get());
            order_pool_.deallocate(order);
        }
    }

    // ========================================================================
    // PROCESS: CANCEL ORDER (Optimized to O(1))
    // ========================================================================
    void process_cancel(OrderId id) {
        current_time_ = Timestamp(current_time_.get() + 1);
        
        // Log event
        if (record_events_) event_log_.emplace_back(std::in_place_type<CancelOrderEvent>,
                              current_time_, id);

        Order* order = order_index_.find(id.get());
        if (!order) {
            if (record_events_) event_log_.emplace_back(std::in_place_type<RejectOrderEvent>,
                                    current_time_, id, RejectReason::ORDER_NOT_FOUND);
            return; // Order not found (already filled or cancelled)
        }

        // 1. Remove from LimitLevel (Intrusive Unlink O(1))
        remove_from_level(order);

        // 2. Free memory
        order_index_.erase(id.get());
        order_pool_.deallocate(order);
    }

    // ========================================================================
    // READ-ONLY ACCESSORS
    // ========================================================================
    std::optional<Price> best_bid() const {
        if (bids_.empty()) return std::nullopt;
        return Price(bids_.begin()->first);
    }

    std::optional<Price> best_ask() const {
        if (asks_.empty()) return std::nullopt;
        return Price(asks_.begin()->first);
    }

      const std::vector<Event>& get_event_log() const {
          return event_log_;
      }

      // Capacity reserved for event capture (does not imply current size).
      size_t event_log_capacity() const noexcept {
          return event_log_.capacity();
      }

      size_t bid_level_count() const noexcept { return bids_.size(); }
      size_t ask_level_count() const noexcept { return asks_.size(); }
      size_t level_pool_capacity() const noexcept { return level_pool_.capacity(); }
      size_t available_level_slots() const noexcept { return level_pool_.available(); }

      // Expensive diagnostic audit. Intended for tests, recovery checks, and
      // low-frequency operational health checks—not the matching hot path.
      bool check_invariants() const {
          if (!bids_.empty() && !asks_.empty() &&
              bids_.begin()->first >= asks_.begin()->first) return false;

          size_t active_orders = 0;
          size_t dense_levels = 0;
          auto check_side = [&](const auto& levels, Side expected_side) {
              for (const auto& [price, level] : levels) {
                  if (!level || level->empty() || level->price.get() != price) return false;
                  if (price_ladder_.contains_price(price)) {
                      if (price_ladder_.find(price) != level) return false;
                      ++dense_levels;
                  }

                  uint64_t volume = 0;
                  size_t count = 0;
                  Order* previous = nullptr;
                  for (Order* order = level->head; order; order = order->next) {
                      if (++count > order_pool_.capacity()) return false; // cycle guard
                      if (order->prev != previous || order->parent_level != level ||
                          order->side != expected_side || order->price.get() != price ||
                          order->remaining_qty.get() == 0 || !order->check_invariants() ||
                          order_index_.find(order->id.get()) != order) return false;
                      if (std::numeric_limits<uint64_t>::max() - volume <
                          order->remaining_qty.get()) return false;
                      volume += order->remaining_qty.get();
                      previous = order;
                  }
                  if (previous != level->tail || count != level->order_count ||
                      volume != level->total_volume.get()) return false;
                  if (std::numeric_limits<size_t>::max() - active_orders < count) return false;
                  active_orders += count;
              }
              return true;
          };

          if (!check_side(bids_, Side::BUY) || !check_side(asks_, Side::SELL)) return false;
          if (active_orders != order_index_.size()) return false;
          if (order_pool_.available() + active_orders != order_pool_.capacity()) return false;
          if (level_pool_.available() + bids_.size() + asks_.size() != level_pool_.capacity()) return false;
          if (price_ladder_.active_count() != dense_levels) return false;
          if (cached_bid_level_ &&
              (cached_bid_level_->price.get() != cached_bid_price_ ||
               bids_.find(cached_bid_price_) != cached_bid_level_)) return false;
          if (cached_ask_level_ &&
              (cached_ask_level_->price.get() != cached_ask_price_ ||
               asks_.find(cached_ask_price_) != cached_ask_level_)) return false;
          return true;
      }

    // Deterministic digest of every live order and its FIFO position.
    uint64_t state_hash() const {
        uint64_t hash = 1469598103934665603ULL;
        auto mix = [&hash](uint64_t value) {
            hash ^= value;
            hash *= 1099511628211ULL;
        };
        auto hash_side = [&mix](const auto& levels, uint64_t side) {
            mix(side);
            for (const auto& [price, level] : levels) {
                mix(static_cast<uint64_t>(price));
                for (Order* order = level->head; order; order = order->next) {
                    mix(order->id.get());
                    mix(order->timestamp.get());
                    mix(order->remaining_qty.get());
                }
            }
        };
        mix(current_time_.get());
        hash_side(bids_, 0);
        hash_side(asks_, 1);
        return hash;
    }

    // Preserve logical time for rejected commands during command replay.
    void advance_replay_time(Timestamp timestamp) {
        if (timestamp.get() > current_time_.get()) {
            current_time_ = timestamp;
        }
    }

    // ========================================================================
    // MATCHING LOGIC
    // ========================================================================
private:
    void match_order_buy(Order* aggressive_order) {
        auto it = asks_.begin();
        
        while (it != asks_.end() && !aggressive_order->is_filled()) {
            Price level_price = Price(it->first);
            LimitLevel& level = *it->second;

            // Check price crossing
            if (aggressive_order->price.get() < level_price.get()) break;

            // Match against orders in the level
            match_level(aggressive_order, level, level_price);

            // If level empty, remove it
            if (level.empty()) {
                if (cached_ask_level_ == &level) {
                    cached_ask_level_ = nullptr;
                    cached_ask_price_ = 0;
                }
                price_ladder_.clear(level_price.get());
                it = asks_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void match_order_sell(Order* aggressive_order) {
        auto it = bids_.begin();
        
        while (it != bids_.end() && !aggressive_order->is_filled()) {
            Price level_price = Price(it->first);
            LimitLevel& level = *it->second;

            if (aggressive_order->price.get() > level_price.get()) break;

            match_level(aggressive_order, level, level_price);

            if (level.empty()) {
                if (cached_bid_level_ == &level) {
                    cached_bid_level_ = nullptr;
                    cached_bid_price_ = 0;
                }
                price_ladder_.clear(level_price.get());
                it = bids_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void match_level(Order* aggressive, LimitLevel& level, Price match_price) {
        while (!level.empty() && !aggressive->is_filled()) {
            Order* passive = level.front(); // O(1) access

            uint64_t trade_qty = std::min(
                aggressive->remaining_qty.get(),
                passive->remaining_qty.get()
            );

            // 1. Generate Trade Event
            current_time_ = Timestamp(current_time_.get() + 1);
            if (record_events_) event_log_.emplace_back(std::in_place_type<TradeEvent>,
                current_time_, passive->id, aggressive->id,
                match_price, Quantity(trade_qty));

            // 2. Update quantities
            aggressive->remaining_qty = Quantity(aggressive->remaining_qty.get() - trade_qty);
            passive->remaining_qty = Quantity(passive->remaining_qty.get() - trade_qty);
            level.total_volume = Quantity(level.total_volume.get() - trade_qty);

            // 3. Handle passive fill
            if (passive->is_filled()) {
                // Remove from book O(1)
                level.pop(); 
                // Remove from index & pool
                order_index_.erase(passive->id.get());
                order_pool_.deallocate(passive);
            }
        }
    }

    // ========================================================================
    // BOOK MANAGEMENT HELPERS
    // ========================================================================
    void add_to_book(Order* order) {
        if (order->side == Side::BUY) {
            if (cached_bid_level_ && cached_bid_price_ == order->price.get()) {
                cached_bid_level_->add_order(order);
                return;
            }
            if (LimitLevel* level = price_ladder_.find(order->price.get())) {
                level->add_order(order);
                cached_bid_level_ = level;
                cached_bid_price_ = order->price.get();
                return;
            }
            auto [level, inserted] = bids_.get_or_create(order->price.get());
            if (!level) {
                std::cerr << "CRITICAL: Price-Level Pool Exhausted despite order/level capacity invariant!\n";
                std::terminate();
            }
            level->add_order(order);
            price_ladder_.set(order->price.get(), level);
            cached_bid_level_ = level;
            cached_bid_price_ = order->price.get();
        } else {
            if (cached_ask_level_ && cached_ask_price_ == order->price.get()) {
                cached_ask_level_->add_order(order);
                return;
            }
            if (LimitLevel* level = price_ladder_.find(order->price.get())) {
                level->add_order(order);
                cached_ask_level_ = level;
                cached_ask_price_ = order->price.get();
                return;
            }
            auto [level, inserted] = asks_.get_or_create(order->price.get());
            if (!level) {
                std::cerr << "CRITICAL: Price-Level Pool Exhausted despite order/level capacity invariant!\n";
                std::terminate();
            }
            level->add_order(order);
            price_ladder_.set(order->price.get(), level);
            cached_ask_level_ = level;
            cached_ask_price_ = order->price.get();
        }
    }

    // O(1) removal from doubly-linked list
    void remove_from_level(Order* order) {
        LimitLevel* level = order->parent_level;
        if (!level) return; // Should not happen

        // Unlink from list
        if (order->prev) order->prev->next = order->next;
        else level->head = order->next; // Was head

        if (order->next) order->next->prev = order->prev;
        else level->tail = order->prev; // Was tail

        // Update level stats
        level->total_volume = Quantity(level->total_volume.get() - order->remaining_qty.get());
        level->order_count--;

        // Clean up level if empty
        if (level->empty()) {
            if (order->side == Side::BUY) {
                bids_.erase(order->price.get());
                price_ladder_.clear(order->price.get());
                if (cached_bid_level_ == level) {
                    cached_bid_level_ = nullptr;
                    cached_bid_price_ = 0;
                }
            } else {
                asks_.erase(order->price.get());
                price_ladder_.clear(order->price.get());
                if (cached_ask_level_ == level) {
                    cached_ask_level_ = nullptr;
                    cached_ask_price_ = 0;
                }
            }
        }

        order->next = nullptr;
        order->prev = nullptr;
        order->parent_level = nullptr;
    }
};

#endif
