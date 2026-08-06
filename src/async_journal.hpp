#ifndef ASYNC_JOURNAL_HPP
#define ASYNC_JOURNAL_HPP

#include "bounded_queue.hpp"
#include "journal.hpp"
#include "submission_gate.hpp"
#include <atomic>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

enum class JournalHealth : uint8_t {
    HEALTHY = 0,
    QUEUE_OVERFLOW = 1,
    WRITE_FAILURE = 2,
    STOPPED = 3
};

// Asynchronous append-only journal writer for one process session. It is not a
// claim of fsync-level crash durability: healthy()==true proves that every
// accepted in-process record was written and flushed during an orderly stop.
class AsyncJournalWriter : public SubmissionGate {
    BoundedQueue<JournalRecord> queue_;
    std::ofstream file_;
    std::thread worker_;
    EventDispatcher::Subscription subscription_;
    std::atomic<uint64_t> accepted_{0};
    std::atomic<uint64_t> written_{0};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<JournalHealth> health_{JournalHealth::HEALTHY};
    std::atomic<bool> stopped_{false};
    uint64_t next_sequence_ = 1; // on_event executes on the single writer path

    void run() noexcept {
        JournalRecord record;
        while (queue_.wait_pop(record)) {
            write_journal_record(file_, record);
            if (!file_) {
                health_.store(JournalHealth::WRITE_FAILURE, std::memory_order_release);
            } else {
                written_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        file_.flush();
        if (!file_) health_.store(JournalHealth::WRITE_FAILURE, std::memory_order_release);
        file_.close();
    }

    void on_event(const SystemEvent& event) {
        if (health_.load(std::memory_order_acquire) != JournalHealth::HEALTHY) return;
        auto record = JournalProjector::project(event, next_sequence_);
        if (!record) return;
        if (!queue_.try_push(std::move(*record))) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            health_.store(JournalHealth::QUEUE_OVERFLOW, std::memory_order_release);
            return;
        }
        ++next_sequence_;
        accepted_.fetch_add(1, std::memory_order_relaxed);
    }

public:
    AsyncJournalWriter(const std::string& filename, size_t queue_capacity)
        : queue_(queue_capacity), file_(filename, std::ios::out | std::ios::trunc) {
        if (!file_) throw std::runtime_error("Cannot open async journal: " + filename);
        write_journal_header(file_);
        worker_ = std::thread([this] { run(); });
    }

    AsyncJournalWriter(const AsyncJournalWriter&) = delete;
    AsyncJournalWriter& operator=(const AsyncJournalWriter&) = delete;
    ~AsyncJournalWriter() { stop(); }

    void attach(EventDispatcher& dispatcher) {
        subscription_ = dispatcher.subscribe_all(
            [this](const SystemEvent& event) { on_event(event); });
    }

    void stop() noexcept {
        bool expected = false;
        if (!stopped_.compare_exchange_strong(expected, true,
                                               std::memory_order_acq_rel)) return;
        subscription_.reset();
        queue_.close();
        if (worker_.joinable()) worker_.join();
        JournalHealth expected_health = JournalHealth::HEALTHY;
        health_.compare_exchange_strong(expected_health, JournalHealth::STOPPED,
                                        std::memory_order_acq_rel);
    }

    bool healthy() const noexcept {
        const JournalHealth current = health();
        return (current == JournalHealth::HEALTHY || current == JournalHealth::STOPPED) &&
               dropped_count() == 0 && !write_failed() &&
               accepted_count() == written_count();
    }
    bool available() const noexcept override {
        return health() == JournalHealth::HEALTHY;
    }
    JournalHealth health() const noexcept {
        return health_.load(std::memory_order_acquire);
    }
    bool write_failed() const noexcept {
        return health() == JournalHealth::WRITE_FAILURE;
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
};

#endif
