#ifndef ASYNC_JOURNAL_HPP
#define ASYNC_JOURNAL_HPP

#include "bounded_queue.hpp"
#include "journal.hpp"
#include <atomic>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

// Asynchronous append-only journal writer for one process session. It is not a
// claim of fsync-level crash durability: healthy()==true proves that every
// accepted in-process record was written and flushed during an orderly stop.
class AsyncJournalWriter {
    BoundedQueue<JournalRecord> queue_;
    Journal projection_;
    std::ofstream file_;
    std::thread worker_;
    EventDispatcher::Subscription subscription_;
    std::atomic<uint64_t> accepted_{0};
    std::atomic<uint64_t> written_{0};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<bool> write_failed_{false};
    std::atomic<bool> stopped_{false};

    void run() noexcept {
        JournalRecord record;
        while (queue_.wait_pop(record)) {
            write_journal_record(file_, record);
            if (!file_) {
                write_failed_.store(true, std::memory_order_release);
            } else {
                written_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        file_.flush();
        if (!file_) write_failed_.store(true, std::memory_order_release);
        file_.close();
    }

    void on_event(const SystemEvent& event) {
        const size_t before = projection_.records().size();
        projection_.on_event(event);
        if (projection_.records().size() == before) return;
        const JournalRecord& record = projection_.records().back();
        if (!queue_.try_push(record)) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
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
    }

    bool healthy() const noexcept {
        return dropped_count() == 0 && !write_failed() &&
               accepted_count() == written_count();
    }
    bool write_failed() const noexcept {
        return write_failed_.load(std::memory_order_acquire);
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
