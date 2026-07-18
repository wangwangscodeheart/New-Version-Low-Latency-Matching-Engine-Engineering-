#ifndef SNAPSHOT_RECOVERY_HPP
#define SNAPSHOT_RECOVERY_HPP

#include "orderbook.hpp"
#include "snapshot.hpp"
#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

struct SnapshotRestoreResult {
    SnapshotRestoreError error = SnapshotRestoreError::NONE;
    std::unique_ptr<OrderBook> book;

    bool success() const noexcept {
        return error == SnapshotRestoreError::NONE && book != nullptr;
    }
};

class SnapshotRecovery {
private:
    static SnapshotRestoreError validate_price_ladder(
        const PriceLadderConfig& config) noexcept {
        if (config.tick <= 0 || config.max_price < config.min_price) {
            return SnapshotRestoreError::INVALID_PRICE_LADDER_CONFIG;
        }
        if (config.min_price < 0 && config.max_price >= 0 &&
            config.max_price > std::numeric_limits<int64_t>::max() + config.min_price) {
            return SnapshotRestoreError::INVALID_PRICE_LADDER_CONFIG;
        }
        const int64_t span = config.max_price - config.min_price;
        if (span % config.tick != 0) {
            return SnapshotRestoreError::INVALID_PRICE_LADDER_CONFIG;
        }
        return SnapshotRestoreError::NONE;
    }

    static SnapshotRestoreError validate_orders(const OrderBookSnapshot& snapshot,
                                                size_t capacity) {
        if (capacity > (std::numeric_limits<size_t>::max() - 1) / 2) {
            return SnapshotRestoreError::CAPACITY_EXCEEDED;
        }
        if (snapshot.active_orders.size() > capacity) {
            return SnapshotRestoreError::CAPACITY_EXCEEDED;
        }

        std::unordered_set<uint64_t> order_ids;
        std::unordered_set<uint64_t> priority_sequences;
        const size_t reserve_hint =
            snapshot.active_orders.size() <=
                    (std::numeric_limits<size_t>::max() - 1) / 2
                ? snapshot.active_orders.size() * 2 + 1
                : snapshot.active_orders.size();
        order_ids.reserve(reserve_hint);
        priority_sequences.reserve(reserve_hint);
        std::map<std::pair<uint8_t, int64_t>, uint64_t> level_volumes;
        std::optional<int64_t> best_bid;
        std::optional<int64_t> best_ask;

        for (const SnapshotOrder& order : snapshot.active_orders) {
            if (order.order_id.get() == 0 || !order_ids.insert(order.order_id.get()).second) {
                return SnapshotRestoreError::DUPLICATE_ORDER_ID;
            }
            if (order.side != Side::BUY && order.side != Side::SELL) {
                return SnapshotRestoreError::INVALID_SIDE;
            }
            if (order.price.get() <= 0 ||
                !snapshot.instrument_config.price_in_range(order.price.get()) ||
                !snapshot.instrument_config.price_on_tick(order.price.get())) {
                return SnapshotRestoreError::INVALID_PRICE;
            }
            if (!snapshot.instrument_config.quantity_in_range(
                    order.original_quantity.get()) ||
                !snapshot.instrument_config.quantity_on_lot(
                    order.original_quantity.get()) ||
                order.remaining_quantity.get() == 0 ||
                order.remaining_quantity.get() > order.original_quantity.get() ||
                !snapshot.instrument_config.quantity_in_range(
                    order.remaining_quantity.get()) ||
                !snapshot.instrument_config.quantity_on_lot(
                    order.remaining_quantity.get())) {
                return SnapshotRestoreError::INVALID_QUANTITY;
            }
            if (order.priority_sequence.get() == 0 ||
                order.priority_sequence.get() >
                    snapshot.last_applied_command_sequence.get()) {
                return SnapshotRestoreError::INVALID_PRIORITY_SEQUENCE;
            }
            if (!priority_sequences.insert(order.priority_sequence.get()).second) {
                return SnapshotRestoreError::DUPLICATE_PRIORITY_SEQUENCE;
            }

            const auto level_key = std::make_pair(
                static_cast<uint8_t>(order.side), order.price.get());
            uint64_t& level_volume = level_volumes[level_key];
            if (std::numeric_limits<uint64_t>::max() - level_volume <
                order.remaining_quantity.get()) {
                return SnapshotRestoreError::LEVEL_VOLUME_OVERFLOW;
            }
            level_volume += order.remaining_quantity.get();

            if (order.side == Side::BUY) {
                if (!best_bid || order.price.get() > *best_bid) best_bid = order.price.get();
            } else {
                if (!best_ask || order.price.get() < *best_ask) best_ask = order.price.get();
            }
        }

        if (best_bid && best_ask && *best_bid >= *best_ask) {
            return SnapshotRestoreError::CROSSED_BOOK;
        }
        return SnapshotRestoreError::NONE;
    }

public:
    static SnapshotRestoreError validate(const OrderBookSnapshot& snapshot,
                                         size_t capacity) {
        if (snapshot.snapshot_version != CURRENT_SNAPSHOT_VERSION) {
            return SnapshotRestoreError::UNSUPPORTED_VERSION;
        }
        try {
            snapshot.instrument_config.validate();
        } catch (const std::invalid_argument&) {
            return SnapshotRestoreError::INVALID_INSTRUMENT_CONFIG;
        }
        const SnapshotRestoreError ladder_error =
            validate_price_ladder(snapshot.price_ladder_config);
        if (ladder_error != SnapshotRestoreError::NONE) return ladder_error;
        return validate_orders(snapshot, capacity);
    }

    static SnapshotRestoreResult restore(const OrderBookSnapshot& snapshot,
                                         size_t capacity) {
        try {
            const SnapshotRestoreError validation_error = validate(snapshot, capacity);
            if (validation_error != SnapshotRestoreError::NONE) {
                return SnapshotRestoreResult{validation_error, nullptr};
            }

            std::vector<SnapshotOrder> sorted_orders = snapshot.active_orders;
            std::sort(sorted_orders.begin(), sorted_orders.end(),
                [](const SnapshotOrder& left, const SnapshotOrder& right) {
                    if (left.side != right.side) return left.side == Side::BUY;
                    if (left.price != right.price) {
                        return left.side == Side::BUY
                            ? left.price > right.price : left.price < right.price;
                    }
                    return left.priority_sequence < right.priority_sequence;
                });

            auto restored = std::make_unique<OrderBook>(
                capacity, true, snapshot.price_ladder_config, 2,
                snapshot.instrument_config);

            for (const SnapshotOrder& saved_order : sorted_orders) {
                Order* order = restored->order_pool_.allocate();
                if (!order) {
                    return SnapshotRestoreResult{
                        SnapshotRestoreError::INTERNAL_RESTORE_FAILURE, nullptr};
                }
                order->activate_from_snapshot(
                    saved_order.order_id, saved_order.priority_sequence, saved_order.side,
                    saved_order.price, saved_order.original_quantity,
                    saved_order.remaining_quantity);

                const auto reservation =
                    restored->order_index_.prepare_insert(saved_order.order_id.get());
                if (reservation.status != FixedOrderIndex::InsertStatus::AVAILABLE ||
                    !restored->order_index_.commit_insert(
                        reservation, saved_order.order_id.get(), order)) {
                    restored->order_pool_.deallocate(order);
                    return SnapshotRestoreResult{
                        SnapshotRestoreError::INTERNAL_RESTORE_FAILURE, nullptr};
                }

                try {
                    restored->add_to_book(order);
                } catch (...) {
                    restored->order_index_.erase(saved_order.order_id.get());
                    restored->order_pool_.deallocate(order);
                    throw;
                }
            }

            restored->last_applied_command_sequence_ =
                snapshot.last_applied_command_sequence;
            restored->current_command_sequence_ =
                snapshot.last_applied_command_sequence;
            restored->next_event_index_ = 0;
            if (!restored->check_invariants()) {
                return SnapshotRestoreResult{
                    SnapshotRestoreError::INTERNAL_RESTORE_FAILURE, nullptr};
            }
            return SnapshotRestoreResult{SnapshotRestoreError::NONE, std::move(restored)};
        } catch (const std::bad_alloc&) {
            return SnapshotRestoreResult{SnapshotRestoreError::ALLOCATION_FAILURE, nullptr};
        } catch (const std::length_error&) {
            return SnapshotRestoreResult{SnapshotRestoreError::ALLOCATION_FAILURE, nullptr};
        }
    }
};

#endif
