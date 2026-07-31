#ifndef AUDIT_LOG_HPP
#define AUDIT_LOG_HPP

#include "event_dispatcher.hpp"
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

enum class AuditAction : uint8_t {
    CREATE = 0, SUBMIT = 1, TRADE = 2, CANCEL = 3, REJECT = 4
};
enum class AuditStatus : uint8_t {
    CREATED = 0, SUBMITTED = 1, PARTIALLY_FILLED = 2, FILLED = 3,
    CANCELLED = 4, REJECTED = 5
};

inline const char* to_string(AuditAction action) noexcept {
    switch (action) {
        case AuditAction::CREATE: return "CREATE";
        case AuditAction::SUBMIT: return "SUBMIT";
        case AuditAction::TRADE: return "TRADE";
        case AuditAction::CANCEL: return "CANCEL";
        case AuditAction::REJECT: return "REJECT";
    }
    return "UNKNOWN";
}

inline const char* to_string(AuditStatus status) noexcept {
    switch (status) {
        case AuditStatus::CREATED: return "CREATED";
        case AuditStatus::SUBMITTED: return "SUBMITTED";
        case AuditStatus::PARTIALLY_FILLED: return "PARTIALLY_FILLED";
        case AuditStatus::FILLED: return "FILLED";
        case AuditStatus::CANCELLED: return "CANCELLED";
        case AuditStatus::REJECTED: return "REJECTED";
    }
    return "UNKNOWN";
}

struct AuditRecord {
    Timestamp timestamp;
    AuditAction action;
    Symbol symbol;
    OrderId order_id;
    Price price;
    Quantity quantity;
    Quantity remaining_quantity;
    AuditStatus status;
};

class AuditLog {
    struct OrderKey {
        std::string symbol;
        uint64_t order_id;
        bool operator==(const OrderKey& other) const noexcept {
            return symbol == other.symbol && order_id == other.order_id;
        }
    };
    struct OrderKeyHash {
        size_t operator()(const OrderKey& key) const noexcept {
            return std::hash<std::string>{}(key.symbol) ^
                   (std::hash<uint64_t>{}(key.order_id) << 1);
        }
    };

    struct PendingKey {
        std::string symbol;
        uint64_t command_sequence;
        bool operator==(const PendingKey& other) const noexcept {
            return symbol == other.symbol && command_sequence == other.command_sequence;
        }
    };
    struct PendingKeyHash {
        size_t operator()(const PendingKey& key) const noexcept {
            return std::hash<std::string>{}(key.symbol) ^
                   (std::hash<uint64_t>{}(key.command_sequence) << 1);
        }
    };
    struct PendingCommand {
        OrderId order_id;
        Price price;
        Quantity quantity;
        bool created_order_state;
    };

    std::vector<AuditRecord> records_;
    std::unordered_map<OrderKey, uint64_t, OrderKeyHash> remaining_;
    std::unordered_map<PendingKey, PendingCommand, PendingKeyHash> pending_;
    EventDispatcher::Subscription subscription_;

    void reject_pending(Timestamp timestamp, const Symbol& symbol,
                        CommandSequence sequence, OrderId fallback_order_id) {
        const PendingKey key{symbol.value(), sequence.get()};
        const auto it = pending_.find(key);
        const PendingCommand pending = it == pending_.end()
            ? PendingCommand{fallback_order_id, Price(0), Quantity(0), false}
            : it->second;
        records_.push_back(AuditRecord{timestamp, AuditAction::REJECT, symbol,
            pending.order_id, pending.price, pending.quantity, pending.quantity,
            AuditStatus::REJECTED});
        if (pending.created_order_state) {
            remaining_.erase(OrderKey{symbol.value(), pending.order_id.get()});
        }
        if (it != pending_.end()) pending_.erase(it);
    }

    void record_trade(Timestamp timestamp, const Symbol& symbol, OrderId id,
                      Price price, Quantity quantity) {
        const OrderKey key{symbol.value(), id.get()};
        auto it = remaining_.find(key);
        if (it == remaining_.end()) return;
        it->second = quantity.get() >= it->second ? 0 : it->second - quantity.get();
        const AuditStatus status = it->second == 0
            ? AuditStatus::FILLED : AuditStatus::PARTIALLY_FILLED;
        records_.push_back(AuditRecord{timestamp, AuditAction::TRADE, symbol, id,
            price, quantity, Quantity(it->second), status});
        if (it->second == 0) remaining_.erase(it);
    }

public:
    void attach(EventDispatcher& dispatcher) {
        subscription_ = dispatcher.subscribe_all(
            [this](const SystemEvent& event) { on_event(event); });
    }

    void on_event(const SystemEvent& event) {
        if (event.event_type == SystemEventType::ORDER) {
            const Command& command = std::get<OrderEvent>(event.payload).command;
            if (const auto* order = std::get_if<NewOrderCommand>(&command)) {
                const OrderKey key{event.symbol.value(), order->order_id.get()};
                const bool created_state =
                    remaining_.try_emplace(key, order->quantity.get()).second;
                pending_.insert_or_assign(
                    PendingKey{event.symbol.value(), order->command_sequence.get()},
                    PendingCommand{order->order_id, order->price,
                                   order->quantity, created_state});
                records_.push_back(AuditRecord{event.timestamp, AuditAction::CREATE,
                    event.symbol, order->order_id, order->price, order->quantity,
                    order->quantity, AuditStatus::CREATED});
                records_.push_back(AuditRecord{event.timestamp, AuditAction::SUBMIT,
                    event.symbol, order->order_id, order->price, order->quantity,
                    order->quantity, AuditStatus::SUBMITTED});
            } else {
                const auto& cancel = std::get<CancelOrderCommand>(command);
                pending_.insert_or_assign(
                    PendingKey{event.symbol.value(), cancel.command_sequence.get()},
                    PendingCommand{cancel.order_id, Price(0), Quantity(0), false});
            }
        } else if (event.event_type == SystemEventType::TRADE) {
            const auto& trade = std::get<TradeEvent>(
                std::get<EngineEvent>(event.payload));
            record_trade(event.timestamp, event.symbol, trade.passive_order_id,
                         trade.price, trade.quantity);
            record_trade(event.timestamp, event.symbol, trade.aggressive_order_id,
                         trade.price, trade.quantity);
        } else if (event.event_type == SystemEventType::CANCEL) {
            const auto& cancel = std::get<OrderCancelledEvent>(
                std::get<EngineEvent>(event.payload));
            records_.push_back(AuditRecord{event.timestamp, AuditAction::CANCEL,
                event.symbol, cancel.order_id, cancel.price, cancel.cancelled_quantity,
                Quantity(0), AuditStatus::CANCELLED});
            remaining_.erase(OrderKey{event.symbol.value(), cancel.order_id.get()});
        } else if (event.event_type == SystemEventType::ORDER_REJECTED) {
            const auto& rejected = std::get<OrderRejectedEvent>(
                std::get<EngineEvent>(event.payload));
            reject_pending(event.timestamp, event.symbol,
                           rejected.command_sequence, rejected.order_id);
        } else if (event.event_type == SystemEventType::PROCESSING_LATENCY) {
            const auto& latency = std::get<ProcessingLatencyEvent>(event.payload);
            const PendingKey key{event.symbol.value(),
                                 latency.command_sequence.get()};
            if (latency.outcome != CommandOutcome::APPLIED &&
                pending_.find(key) != pending_.end()) {
                reject_pending(event.timestamp, event.symbol,
                               latency.command_sequence, latency.order_id);
            } else {
                pending_.erase(key);
            }
        }
    }

    const std::vector<AuditRecord>& records() const noexcept { return records_; }

    void save(const std::string& filename) const {
        std::ofstream file(filename, std::ios::out | std::ios::trunc);
        if (!file) throw std::runtime_error("Cannot open audit log: " + filename);
        file << "timestamp,action,symbol,order_id,price,quantity,remaining,status\n";
        for (const AuditRecord& record : records_) {
            file << record.timestamp.get() << ',' << to_string(record.action) << ','
                 << record.symbol.value() << ',' << record.order_id.get() << ','
                 << record.price.get() << ',' << record.quantity.get() << ','
                 << record.remaining_quantity.get() << ',' << to_string(record.status)
                 << '\n';
        }
    }
};

#endif
