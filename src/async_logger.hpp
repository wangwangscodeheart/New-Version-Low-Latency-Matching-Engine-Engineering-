#ifndef ASYNC_LOGGER_HPP
#define ASYNC_LOGGER_HPP

#include "bounded_queue.hpp"
#include "event_dispatcher.hpp"
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>

inline const char* to_string(SystemEventType type) noexcept {
    switch (type) {
        case SystemEventType::MARKET_DATA: return "MARKET_DATA";
        case SystemEventType::ORDER: return "ORDER";
        case SystemEventType::TRADE: return "TRADE";
        case SystemEventType::ORDER_RESTED: return "ORDER_RESTED";
        case SystemEventType::CANCEL: return "CANCEL";
        case SystemEventType::ORDER_REJECTED: return "ORDER_REJECTED";
        case SystemEventType::PROCESSING_LATENCY: return "PROCESSING_LATENCY";
        case SystemEventType::DATA_QUALITY: return "DATA_QUALITY";
    }
    return "UNKNOWN";
}

class AsyncLogger {
    BoundedQueue<SystemEvent> queue_;
    std::ofstream file_;
    std::thread worker_;
    std::atomic<uint64_t> accepted_{0};
    std::atomic<uint64_t> written_{0};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<bool> stopped_{false};
    EventDispatcher::Subscription subscription_;

    static std::string payload_to_string(const SystemEvent& event) {
        char buffer[256]{};
        if (const auto* market = std::get_if<MarketDataEvent>(&event.payload)) {
            std::snprintf(buffer, sizeof(buffer),
                "price=%" PRId64 ";volume=%" PRIu64 ";bid=%" PRId64 ";ask=%" PRId64,
                market->price.get(), market->volume.get(), market->bid.get(),
                market->ask.get());
        } else if (const auto* order_event = std::get_if<OrderEvent>(&event.payload)) {
            if (const auto* order = std::get_if<NewOrderCommand>(&order_event->command)) {
                std::snprintf(buffer, sizeof(buffer),
                    "NEW_ORDER;sequence=%" PRIu64 ";order_id=%" PRIu64
                    ";side=%s;price=%" PRId64 ";quantity=%" PRIu64,
                    order->command_sequence.get(), order->order_id.get(),
                    to_string(order->side), order->price.get(), order->quantity.get());
            } else {
                const auto& cancel = std::get<CancelOrderCommand>(order_event->command);
                std::snprintf(buffer, sizeof(buffer),
                    "CANCEL_ORDER;sequence=%" PRIu64 ";order_id=%" PRIu64,
                    cancel.command_sequence.get(), cancel.order_id.get());
            }
        } else if (const auto* latency =
                       std::get_if<ProcessingLatencyEvent>(&event.payload)) {
            std::snprintf(buffer, sizeof(buffer),
                "nanoseconds=%" PRIu64 ";outcome=%u;sequence=%" PRIu64
                ";order_id=%" PRIu64,
                latency->nanoseconds, static_cast<unsigned>(latency->outcome),
                latency->command_sequence.get(),
                latency->order_id.get());
        } else if (const auto* quality =
                       std::get_if<DataQualityEvent>(&event.payload)) {
            std::snprintf(buffer, sizeof(buffer), "severity=%s;code=%u;message=%s",
                quality->severity == DataQualitySeverity::ERROR ? "ERROR" : "WARNING",
                static_cast<unsigned>(quality->code), quality->message.c_str());
        } else {
            event_to_buffer(std::get<EngineEvent>(event.payload), buffer, sizeof(buffer));
        }
        return std::string(buffer);
    }

    void run() {
        SystemEvent event = SystemEvent::market_data(
            Timestamp(0), Symbol("LOGGER"),
            MarketDataEvent{Price(0), Quantity(0), Price(0), Price(0)});
        while (queue_.wait_pop(event)) {
            file_ << event.timestamp.get() << ',' << to_string(event.event_type) << ','
                  << event.symbol.value() << ',' << payload_to_string(event) << '\n';
            written_.fetch_add(1, std::memory_order_relaxed);
        }
        file_.flush();
        file_.close();
    }

public:
    AsyncLogger(const std::string& filename, size_t queue_capacity)
        : queue_(queue_capacity), file_(filename, std::ios::out | std::ios::trunc) {
        if (!file_) throw std::runtime_error("Cannot open async log: " + filename);
        file_ << "timestamp,event_type,symbol,payload\n";
        worker_ = std::thread([this] { run(); });
    }

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    ~AsyncLogger() { stop(); }

    // Logger must remain alive while the dispatcher can publish events.
    void attach(EventDispatcher& dispatcher) {
        subscription_ = dispatcher.subscribe_all(
            [this](const SystemEvent& event) { enqueue(event); });
    }

    bool enqueue(const SystemEvent& event) {
        if (stopped_.load(std::memory_order_acquire) || !queue_.try_push(event)) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        accepted_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void stop() noexcept {
        bool expected = false;
        if (!stopped_.compare_exchange_strong(expected, true,
                                               std::memory_order_acq_rel)) return;
        subscription_.reset();
        queue_.close();
        if (worker_.joinable()) worker_.join();
    }

    uint64_t accepted_count() const noexcept {
        return accepted_.load(std::memory_order_relaxed);
    }
    uint64_t written_count() const noexcept {
        return written_.load(std::memory_order_relaxed);
    }
    uint64_t dropped_count() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }
    size_t queue_capacity() const noexcept { return queue_.capacity(); }
};

#endif
