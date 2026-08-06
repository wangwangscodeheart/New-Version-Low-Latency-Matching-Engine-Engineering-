#ifndef MARKET_MANAGER_HPP
#define MARKET_MANAGER_HPP

#include "orderbook.hpp"
#include "symbol.hpp"
#include "instrument_id.hpp"
#include <cstddef>
#include <memory>
#include <algorithm>
#include <unordered_map>
#include <vector>

struct InstrumentRuntimeConfig {
    size_t order_capacity = 100000;
    bool record_events = true;
    PriceLadderConfig price_ladder{};
    size_t event_reserve_multiplier = 2;
    InstrumentConfig trading_rules{};
};

struct EmptyMarketManagerTag {};

enum class RoutingStatus : uint8_t {
    ROUTED = 0,
    UNKNOWN_SYMBOL = 1,
    SYSTEM_UNAVAILABLE = 2
};

struct MarketProcessResult {
    RoutingStatus routing_status;
    ProcessResult process_result;

    bool routed() const noexcept {
        return routing_status == RoutingStatus::ROUTED;
    }
};

// Owns one independent deterministic OrderBook per symbol. MarketManager is a
// single-writer component: callers must serialize register/process operations.
class MarketManager {
    struct InstrumentSlot {
        Symbol symbol;
        std::unique_ptr<OrderBook> book;
    };
    std::vector<InstrumentSlot> instruments_;
    std::unordered_map<Symbol, uint32_t, SymbolHash> instrument_ids_;

public:
    explicit MarketManager(EmptyMarketManagerTag) noexcept {}

    explicit MarketManager(size_t default_capacity = 100000) {
        InstrumentRuntimeConfig config;
        config.order_capacity = default_capacity;
        register_symbol(Symbol("AAPL"), config);
        register_symbol(Symbol("TSLA"), config);
        register_symbol(Symbol("NVDA"), config);
    }

    bool register_symbol(const Symbol& symbol,
                         const InstrumentRuntimeConfig& config) {
        if (instrument_ids_.find(symbol) != instrument_ids_.end()) return false;
        if (instruments_.size() >= InstrumentId::INVALID_VALUE) return false;
        auto book = std::make_unique<OrderBook>(
            config.order_capacity, config.record_events, config.price_ladder,
            config.event_reserve_multiplier, config.trading_rules);
        const uint32_t id = static_cast<uint32_t>(instruments_.size());
        instruments_.push_back(InstrumentSlot{symbol, std::move(book)});
        instrument_ids_.emplace(symbol, id);
        return true;
    }

    MarketProcessResult process(const Symbol& symbol, const Command& command) {
        OrderBook* book = find_book(symbol);
        if (!book) {
            return MarketProcessResult{
                RoutingStatus::UNKNOWN_SYMBOL,
                ProcessResult{ProcessStatus::SEQUENCE_REJECTED, 0, 0}};
        }
        return MarketProcessResult{RoutingStatus::ROUTED, book->process(command)};
    }

    template<typename EventSink>
    MarketProcessResult process(const Symbol& symbol, const Command& command,
                                EventSink& sink) {
        OrderBook* book = find_book(symbol);
        if (!book) {
            return MarketProcessResult{RoutingStatus::UNKNOWN_SYMBOL,
                ProcessResult{ProcessStatus::SEQUENCE_REJECTED, 0, 0}};
        }
        return MarketProcessResult{RoutingStatus::ROUTED,
                                   book->process(command, sink)};
    }

    template<typename EventSink>
    MarketProcessResult process(InstrumentId id, const Command& command,
                                EventSink& sink) {
        OrderBook* book = find_book(id);
        if (!book) {
            return MarketProcessResult{RoutingStatus::UNKNOWN_SYMBOL,
                ProcessResult{ProcessStatus::SEQUENCE_REJECTED, 0, 0}};
        }
        return MarketProcessResult{RoutingStatus::ROUTED, book->process(command, sink)};
    }

    InstrumentId resolve(const Symbol& symbol) const noexcept {
        const auto found = instrument_ids_.find(symbol);
        return found == instrument_ids_.end()
            ? InstrumentId{} : InstrumentId(found->second);
    }

    const Symbol* symbol(InstrumentId id) const noexcept {
        return id.valid() && id.get() < instruments_.size()
            ? &instruments_[id.get()].symbol : nullptr;
    }

    OrderBook* find_book(const Symbol& symbol) noexcept {
        return find_book(resolve(symbol));
    }

    const OrderBook* find_book(const Symbol& symbol) const noexcept {
        return find_book(resolve(symbol));
    }

    OrderBook* find_book(InstrumentId id) noexcept {
        return id.valid() && id.get() < instruments_.size()
            ? instruments_[id.get()].book.get() : nullptr;
    }
    const OrderBook* find_book(InstrumentId id) const noexcept {
        return id.valid() && id.get() < instruments_.size()
            ? instruments_[id.get()].book.get() : nullptr;
    }

    bool contains(const Symbol& symbol) const noexcept {
        return find_book(symbol) != nullptr;
    }

    size_t symbol_count() const noexcept { return instruments_.size(); }

    // Recovery builds every book off to the side and publishes it only after
    // complete validation. This method is intentionally not used by routing.
    void install_restored_book(const Symbol& symbol,
                               std::unique_ptr<OrderBook> book) {
        if (!book) throw std::invalid_argument("Restored OrderBook is null");
        const InstrumentId existing = resolve(symbol);
        if (existing.valid()) {
            instruments_[existing.get()].book = std::move(book);
            return;
        }
        if (instruments_.size() >= InstrumentId::INVALID_VALUE) {
            throw std::overflow_error("Too many restored instruments");
        }
        const uint32_t id = static_cast<uint32_t>(instruments_.size());
        instruments_.push_back(InstrumentSlot{symbol, std::move(book)});
        instrument_ids_.emplace(symbol, id);
    }

    std::vector<Symbol> symbols() const {
        std::vector<Symbol> result;
        result.reserve(instruments_.size());
        for (const auto& entry : instruments_) result.push_back(entry.symbol);
        std::sort(result.begin(), result.end(),
            [](const Symbol& left, const Symbol& right) {
                return left.value() < right.value();
            });
        return result;
    }
};

#endif
