#ifndef BOOK_STATE_HPP
#define BOOK_STATE_HPP

#include "types.hpp"
#include <cstddef>
#include <optional>
#include <vector>

struct BookOrderState {
    OrderId order_id;
    Side side;
    Price price;
    Quantity original_quantity;
    Quantity remaining_quantity;
    PrioritySequence priority_sequence;

    bool operator==(const BookOrderState& other) const noexcept {
        return order_id == other.order_id && side == other.side && price == other.price &&
               original_quantity == other.original_quantity &&
               remaining_quantity == other.remaining_quantity &&
               priority_sequence == other.priority_sequence;
    }
};

struct BookLevelState {
    Side side;
    Price price;
    Quantity total_volume;
    std::vector<BookOrderState> orders;

    bool operator==(const BookLevelState& other) const noexcept {
        return side == other.side && price == other.price &&
               total_volume == other.total_volume && orders == other.orders;
    }
};

struct OrderBookState {
    CommandSequence last_applied_command_sequence{0};
    std::vector<BookLevelState> bid_levels;
    std::vector<BookLevelState> ask_levels;
    size_t active_order_count = 0;
    std::optional<Price> best_bid;
    std::optional<Price> best_ask;

    bool operator==(const OrderBookState& other) const noexcept {
        return last_applied_command_sequence == other.last_applied_command_sequence &&
               bid_levels == other.bid_levels && ask_levels == other.ask_levels &&
               active_order_count == other.active_order_count &&
               best_bid == other.best_bid && best_ask == other.best_ask;
    }
};

#endif
