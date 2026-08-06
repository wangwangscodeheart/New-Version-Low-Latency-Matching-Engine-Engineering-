#ifndef REFERENCE_ENGINE_HPP
#define REFERENCE_ENGINE_HPP

#include "book_state.hpp"
#include "commands.hpp"
#include "events.hpp"
#include "instrument_config.hpp"
#include "process_status.hpp"
#include <algorithm>
#include <deque>
#include <map>
#include <unordered_map>
#include <vector>

// Deliberately simple correctness oracle. It favors obvious STL operations
// over the pools, intrusive queues and fixed hash index used by OrderBook.
class ReferenceEngine {
    struct ReferenceOrder {
        OrderId id;
        Side side;
        Price price;
        Quantity original;
        Quantity remaining;
        PrioritySequence priority;
    };

    using BidBook = std::map<int64_t, std::deque<ReferenceOrder>, std::greater<int64_t>>;
    using AskBook = std::map<int64_t, std::deque<ReferenceOrder>, std::less<int64_t>>;

public:
    explicit ReferenceEngine(size_t capacity = 1'000'000,
                             InstrumentConfig config = {})
        : capacity_(capacity), config_(config) {
        config_.validate();
    }

    ProcessStatus process(const Command& command, std::vector<EngineEvent>& output) {
        output.clear();
        const CommandSequence sequence = get_command_sequence(command);
        if (sequence.get() == 0 || sequence.get() <= last_sequence_.get()) {
            return ProcessStatus::SEQUENCE_REJECTED;
        }
        current_sequence_ = sequence;
        next_event_index_ = 0;
        const ProcessStatus status = std::visit([this, &output](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, NewOrderCommand>) {
                return process_new(value, output);
            } else {
                return process_cancel(value, output);
            }
        }, command);
        last_sequence_ = sequence;
        return status;
    }

    OrderBookState capture_state() const {
        OrderBookState state;
        state.last_applied_command_sequence = last_sequence_;
        append_levels(bids_, Side::BUY, state.bid_levels);
        append_levels(asks_, Side::SELL, state.ask_levels);
        state.active_order_count = locations_.size();
        if (!bids_.empty()) state.best_bid = Price(bids_.begin()->first);
        if (!asks_.empty()) state.best_ask = Price(asks_.begin()->first);
        return state;
    }

private:
    struct Location { Side side; int64_t price; };

    void reject(std::vector<EngineEvent>& output, CommandType type,
                OrderId id, RejectReason reason) {
        output.emplace_back(std::in_place_type<OrderRejectedEvent>,
                            current_sequence_, EventIndex(next_event_index_++),
                            type, id, reason);
    }

    ProcessStatus process_new(const NewOrderCommand& command,
                              std::vector<EngineEvent>& output) {
        if (command.order_id.get() == 0 || locations_.count(command.order_id.get()) != 0) {
            reject(output, CommandType::NEW_ORDER, command.order_id,
                   RejectReason::DUPLICATE_ORDER_ID);
            return ProcessStatus::REJECTED;
        }
        if (command.price.get() <= 0) {
            reject(output, CommandType::NEW_ORDER, command.order_id, RejectReason::INVALID_PRICE);
            return ProcessStatus::REJECTED;
        }
        if (!config_.price_in_range(command.price.get())) {
            reject(output, CommandType::NEW_ORDER, command.order_id, RejectReason::PRICE_OUT_OF_RANGE);
            return ProcessStatus::REJECTED;
        }
        if (!config_.price_on_tick(command.price.get())) {
            reject(output, CommandType::NEW_ORDER, command.order_id, RejectReason::OFF_TICK_PRICE);
            return ProcessStatus::REJECTED;
        }
        if (command.quantity.get() == 0) {
            reject(output, CommandType::NEW_ORDER, command.order_id, RejectReason::INVALID_QUANTITY);
            return ProcessStatus::REJECTED;
        }
        if (!config_.quantity_in_range(command.quantity.get())) {
            reject(output, CommandType::NEW_ORDER, command.order_id, RejectReason::QUANTITY_LIMIT);
            return ProcessStatus::REJECTED;
        }
        if (!config_.quantity_on_lot(command.quantity.get())) {
            reject(output, CommandType::NEW_ORDER, command.order_id, RejectReason::INVALID_LOT_SIZE);
            return ProcessStatus::REJECTED;
        }
        if (locations_.size() >= capacity_) {
            reject(output, CommandType::NEW_ORDER, command.order_id, RejectReason::POOL_EXHAUSTED);
            return ProcessStatus::REJECTED;
        }

        ReferenceOrder aggressive{command.order_id, command.side, command.price,
                                  command.quantity, command.quantity,
                                  PrioritySequence(current_sequence_.get())};
        if (command.side == Side::BUY) match_buy(aggressive, output);
        else match_sell(aggressive, output);

        if (aggressive.remaining.get() != 0) {
            rest(aggressive);
            output.emplace_back(std::in_place_type<OrderRestedEvent>, current_sequence_,
                                EventIndex(next_event_index_++), aggressive.id,
                                aggressive.side, aggressive.price, aggressive.original,
                                aggressive.remaining, aggressive.priority);
        }
        return ProcessStatus::APPLIED;
    }

    template<typename OppositeBook, typename Crosses>
    void match(ReferenceOrder& aggressive, OppositeBook& opposite, Crosses crosses,
               std::vector<EngineEvent>& output) {
        while (aggressive.remaining.get() != 0 && !opposite.empty() &&
               crosses(opposite.begin()->first)) {
            auto level = opposite.begin();
            auto& passive = level->second.front();
            const uint64_t quantity = std::min(aggressive.remaining.get(), passive.remaining.get());
            aggressive.remaining = Quantity(aggressive.remaining.get() - quantity);
            passive.remaining = Quantity(passive.remaining.get() - quantity);
            output.emplace_back(std::in_place_type<TradeEvent>, current_sequence_,
                                EventIndex(next_event_index_++), passive.id, aggressive.id,
                                passive.price, Quantity(quantity));
            if (passive.remaining.get() == 0) {
                locations_.erase(passive.id.get());
                level->second.pop_front();
                if (level->second.empty()) opposite.erase(level);
            }
        }
    }

    void match_buy(ReferenceOrder& order, std::vector<EngineEvent>& output) {
        match(order, asks_, [&order](int64_t price) { return price <= order.price.get(); }, output);
    }
    void match_sell(ReferenceOrder& order, std::vector<EngineEvent>& output) {
        match(order, bids_, [&order](int64_t price) { return price >= order.price.get(); }, output);
    }

    void rest(const ReferenceOrder& order) {
        if (order.side == Side::BUY) bids_[order.price.get()].push_back(order);
        else asks_[order.price.get()].push_back(order);
        locations_.emplace(order.id.get(), Location{order.side, order.price.get()});
    }

    ProcessStatus process_cancel(const CancelOrderCommand& command,
                                 std::vector<EngineEvent>& output) {
        const auto found = locations_.find(command.order_id.get());
        if (found == locations_.end()) {
            reject(output, CommandType::CANCEL_ORDER, command.order_id,
                   RejectReason::ORDER_NOT_FOUND);
            return ProcessStatus::REJECTED;
        }
        if (found->second.side == Side::BUY) cancel_from(bids_, found, output);
        else cancel_from(asks_, found, output);
        return ProcessStatus::APPLIED;
    }

    template<typename Book>
    void cancel_from(Book& book,
                     std::unordered_map<uint64_t, Location>::iterator location,
                     std::vector<EngineEvent>& output) {
        auto level = book.find(location->second.price);
        auto order = std::find_if(level->second.begin(), level->second.end(),
                                  [id = location->first](const ReferenceOrder& value) {
                                      return value.id.get() == id;
                                  });
        output.emplace_back(std::in_place_type<OrderCancelledEvent>, current_sequence_,
                            EventIndex(next_event_index_++), order->id, order->side,
                            order->price, order->remaining, order->priority);
        level->second.erase(order);
        if (level->second.empty()) book.erase(level);
        locations_.erase(location);
    }

    template<typename Book>
    static void append_levels(const Book& book, Side side,
                              std::vector<BookLevelState>& destination) {
        for (const auto& [price, orders] : book) {
            BookLevelState level{side, Price(price), Quantity(0), {}};
            uint64_t total = 0;
            for (const auto& order : orders) {
                total += order.remaining.get();
                level.orders.push_back(BookOrderState{order.id, order.side, order.price,
                    order.original, order.remaining, order.priority});
            }
            level.total_volume = Quantity(total);
            destination.push_back(std::move(level));
        }
    }

    size_t capacity_;
    InstrumentConfig config_;
    BidBook bids_;
    AskBook asks_;
    std::unordered_map<uint64_t, Location> locations_;
    CommandSequence last_sequence_{0};
    CommandSequence current_sequence_{0};
    uint32_t next_event_index_ = 0;
};

#endif
