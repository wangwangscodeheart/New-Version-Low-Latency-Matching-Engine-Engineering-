#ifndef MONITOR_HPP
#define MONITOR_HPP

#include "async_logger.hpp"
#include "event_dispatcher.hpp"
#include "market_manager.hpp"
#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

struct TradingMetrics {
    uint64_t orders = 0;
    uint64_t trades = 0;
    uint64_t cancels = 0;
    uint64_t rejects = 0;
    uint64_t traded_volume = 0;
    uint64_t latency_samples = 0;
    uint64_t total_latency_ns = 0;
    uint64_t max_latency_ns = 0;
    uint64_t data_warnings = 0;
    uint64_t data_errors = 0;

    double average_latency_ns() const noexcept {
        return latency_samples == 0 ? 0.0 :
            static_cast<double>(total_latency_ns) /
            static_cast<double>(latency_samples);
    }
};

struct MonitorSnapshot {
    TradingMetrics global;
    std::unordered_map<std::string, TradingMetrics> by_symbol;
    uint64_t dropped_logs = 0;
};

class Monitor {
    mutable std::mutex mutex_;
    TradingMetrics global_;
    std::vector<TradingMetrics> by_instrument_;
    std::vector<std::string> instrument_symbols_;
    std::unordered_map<std::string, TradingMetrics> fallback_by_symbol_;
    EventDispatcher::Subscription subscription_;

    static void add_latency(TradingMetrics& metrics, uint64_t latency) noexcept {
        ++metrics.latency_samples;
        metrics.total_latency_ns += latency;
        metrics.max_latency_ns = std::max(metrics.max_latency_ns, latency);
    }

public:
    void attach(EventDispatcher& dispatcher, const MarketManager* markets = nullptr) {
        if (markets) {
            by_instrument_.resize(markets->symbol_count());
            instrument_symbols_.resize(markets->symbol_count());
            for (const Symbol& symbol : markets->symbols()) {
                const InstrumentId id = markets->resolve(symbol);
                instrument_symbols_[id.get()] = symbol.value();
            }
        }
        subscription_ = dispatcher.subscribe_all(
            [this](const SystemEvent& event) { on_event(event); });
    }

    void on_event(const SystemEvent& event) {
        std::lock_guard<std::mutex> lock(mutex_);
        TradingMetrics& symbol = event.instrument_id.valid() &&
                event.instrument_id.get() < by_instrument_.size()
            ? by_instrument_[event.instrument_id.get()]
            : fallback_by_symbol_[event.symbol.value()];
        if (event.event_type == SystemEventType::ORDER) {
            const Command& command = std::get<OrderEvent>(event.payload).command;
            if (std::holds_alternative<NewOrderCommand>(command)) {
                ++global_.orders;
                ++symbol.orders;
            }
        } else if (event.event_type == SystemEventType::TRADE) {
            const auto& trade = std::get<TradeEvent>(
                std::get<EngineEvent>(event.payload));
            ++global_.trades;
            ++symbol.trades;
            global_.traded_volume += trade.quantity.get();
            symbol.traded_volume += trade.quantity.get();
        } else if (event.event_type == SystemEventType::CANCEL) {
            ++global_.cancels;
            ++symbol.cancels;
        } else if (event.event_type == SystemEventType::ORDER_REJECTED) {
            ++global_.rejects;
            ++symbol.rejects;
        } else if (event.event_type == SystemEventType::PROCESSING_LATENCY) {
            const auto& latency = std::get<ProcessingLatencyEvent>(event.payload);
            add_latency(global_, latency.nanoseconds);
            add_latency(symbol, latency.nanoseconds);
        } else if (event.event_type == SystemEventType::DATA_QUALITY) {
            const auto& quality = std::get<DataQualityEvent>(event.payload);
            if (quality.severity == DataQualitySeverity::ERROR) {
                ++global_.data_errors;
                ++symbol.data_errors;
            } else {
                ++global_.data_warnings;
                ++symbol.data_warnings;
            }
        }
    }

    MonitorSnapshot snapshot(const AsyncLogger* logger = nullptr) const {
        std::lock_guard<std::mutex> lock(mutex_);
        MonitorSnapshot result{global_, fallback_by_symbol_, 0};
        for (size_t i = 0; i < by_instrument_.size(); ++i) {
            result.by_symbol[instrument_symbols_[i]] = by_instrument_[i];
        }
        if (logger) result.dropped_logs = logger->dropped_count();
        return result;
    }

    void print(std::ostream& output, const AsyncLogger* logger = nullptr) const {
        const MonitorSnapshot current = snapshot(logger);
        output << "=== Trading System Monitor ===\n"
               << "Global: orders=" << current.global.orders
               << " trades=" << current.global.trades
               << " cancels=" << current.global.cancels
               << " rejects=" << current.global.rejects
               << " volume=" << current.global.traded_volume
               << " avg_latency_ns=" << std::fixed << std::setprecision(2)
               << current.global.average_latency_ns()
               << " max_latency_ns=" << current.global.max_latency_ns
               << " data_warnings=" << current.global.data_warnings
               << " data_errors=" << current.global.data_errors
               << " dropped_logs=" << current.dropped_logs << '\n';
        for (const auto& entry : current.by_symbol) {
            const TradingMetrics& metrics = entry.second;
            output << entry.first << ": orders=" << metrics.orders
                   << " trades=" << metrics.trades
                   << " cancels=" << metrics.cancels
                   << " rejects=" << metrics.rejects
                   << " volume=" << metrics.traded_volume
                   << " avg_latency_ns=" << metrics.average_latency_ns()
                   << " max_latency_ns=" << metrics.max_latency_ns << '\n';
        }
    }
};

#endif
