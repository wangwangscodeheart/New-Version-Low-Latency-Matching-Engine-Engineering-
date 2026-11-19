#ifndef ASYNC_LOGGER_HPP
#define ASYNC_LOGGER_HPP

#include "bounded_queue.hpp"
#include "event_dispatcher.hpp"
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <ostream>
#include <new>
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

enum class LoggerHealth : uint8_t {
    HEALTHY = 0, WRITE_FAILURE, INTERNAL_FAILURE, ALLOCATION_FAILURE, STOPPED
};

inline const char* to_string(LoggerHealth health) noexcept {
    switch (health) {
        case LoggerHealth::HEALTHY: return "HEALTHY";
        case LoggerHealth::WRITE_FAILURE: return "WRITE_FAILURE";
        case LoggerHealth::INTERNAL_FAILURE: return "INTERNAL_FAILURE";
        case LoggerHealth::ALLOCATION_FAILURE: return "ALLOCATION_FAILURE";
        case LoggerHealth::STOPPED: return "STOPPED";
    }
    return "UNKNOWN";
}

// Best-effort diagnostic events, NOT the recovery-critical CommandJournal or
// the in-memory AuditLog lifecycle view. Quiesce producers/dispatcher before
// stop/destruction; shutdown is controlled by one thread, not a concurrent join.
class AsyncLogger {
    BoundedQueue<SystemEvent> queue_;
    std::ofstream file_;
    std::ostream* output_;
    std::thread worker_;
    std::atomic<uint64_t> accepted_{0};
    std::atomic<uint64_t> written_{0};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> failed_{0};
    std::atomic<LoggerHealth> health_{LoggerHealth::HEALTHY};
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

    void fail(LoggerHealth reason) noexcept {
        if (reason == LoggerHealth::WRITE_FAILURE) {
            health_.store(reason, std::memory_order_release);
        } else {
            LoggerHealth expected = LoggerHealth::HEALTHY;
            health_.compare_exchange_strong(expected, reason, std::memory_order_acq_rel);
        }
    }

    void run() noexcept {
        bool stream_failed = false;
        try {
            SystemEvent event = SystemEvent::market_data(
                Timestamp(0), Symbol("LOGGER"),
                MarketDataEvent{Price(0), Quantity(0), Price(0), Price(0)});
            while (queue_.wait_pop(event)) {
                if (!stream_failed) {
                    std::string payload;
                    try { payload = payload_to_string(event); }
                    catch (const std::bad_alloc&) {
                        fail(LoggerHealth::ALLOCATION_FAILURE);
                        stream_failed = true;
                    } catch (...) {
                        fail(LoggerHealth::INTERNAL_FAILURE);
                        stream_failed = true;
                    }
                    if (!stream_failed) {
                        try {
                            *output_ << event.timestamp.get() << ',' << to_string(event.event_type) << ','
                                     << event.symbol.value() << ',' << payload << '\n';
                            if (!*output_) stream_failed = true;
                        } catch (...) { stream_failed = true; }
                        if (stream_failed) fail(LoggerHealth::WRITE_FAILURE);
                    }
                }
                if (stream_failed) failed_.fetch_add(1, std::memory_order_relaxed);
                else written_.fetch_add(1, std::memory_order_relaxed);
            }
        } catch (const std::bad_alloc&) { fail(LoggerHealth::ALLOCATION_FAILURE); }
        catch (...) { fail(LoggerHealth::INTERNAL_FAILURE); }
        try {
            output_->flush();
            if (!*output_) fail(LoggerHealth::WRITE_FAILURE);
        } catch (...) { fail(LoggerHealth::WRITE_FAILURE); }
        try {
            if (file_.is_open()) {
                file_.close();
                if (file_.fail()) fail(LoggerHealth::WRITE_FAILURE);
            }
        } catch (...) { fail(LoggerHealth::WRITE_FAILURE); }
    }

    void start() {
        *output_ << "timestamp,event_type,symbol,payload\n";
        output_->flush(); // Cold initialization only; event enqueue never does I/O.
        if (!*output_) throw std::runtime_error("Cannot write async log header");
        worker_ = std::thread([this] { run(); });
    }

public:
    AsyncLogger(const std::string& filename, size_t queue_capacity)
        : queue_(queue_capacity), file_(filename, std::ios::out | std::ios::trunc), output_(&file_) {
        if (!file_) throw std::runtime_error("Cannot open async log: " + filename);
        start();
    }

    // Borrowed stream must outlive this logger through stop(), with no other
    // concurrent stream access. Only an owned file is closed by the logger.
    AsyncLogger(std::ostream& output, size_t queue_capacity)
        : queue_(queue_capacity), output_(&output) { start(); }

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    ~AsyncLogger() { stop(); }

    // Logger must remain alive while the dispatcher can publish events.
    void attach(EventDispatcher& dispatcher) {
        if (!available()) throw std::logic_error("Cannot attach an unavailable logger");
        subscription_ = dispatcher.subscribe_all(
            [this](const SystemEvent& event) { enqueue(event); });
    }

    bool enqueue(const SystemEvent& event) {
        if (available()) {
            try {
                if (queue_.try_push(event)) {
                    accepted_.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }
            } catch (const std::bad_alloc&) { fail(LoggerHealth::ALLOCATION_FAILURE); }
            catch (...) { fail(LoggerHealth::INTERNAL_FAILURE); }
        }
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    void stop() noexcept {
        bool expected = false;
        if (!stopped_.compare_exchange_strong(expected, true,
                                               std::memory_order_acq_rel)) return;
        subscription_.reset();
        queue_.close();
        if (worker_.joinable()) worker_.join();
        // Final accounting includes any queue tail left by an unexpected
        // worker infrastructure exception. Producers must already be quiescent.
        failed_.store(accepted_count() - written_count(), std::memory_order_relaxed);
        LoggerHealth running = LoggerHealth::HEALTHY;
        health_.compare_exchange_strong(running, LoggerHealth::STOPPED, std::memory_order_acq_rel);
    }

    LoggerHealth health() const noexcept { return health_.load(std::memory_order_acquire); }
    // Stream/worker health, not a claim that dropped==0 or output is durable.
    bool healthy() const noexcept {
        const auto current = health();
        return current == LoggerHealth::HEALTHY || current == LoggerHealth::STOPPED;
    }
    bool available() const noexcept {
        return !stopped_.load(std::memory_order_acquire) && health() == LoggerHealth::HEALTHY;
    }
    uint64_t failed_count() const noexcept { return failed_.load(std::memory_order_relaxed); }
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
