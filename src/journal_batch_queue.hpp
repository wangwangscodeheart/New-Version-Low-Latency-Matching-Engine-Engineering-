#ifndef JOURNAL_BATCH_QUEUE_HPP
#define JOURNAL_BATCH_QUEUE_HPP

#include "journal.hpp"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>

// Record-slot budget includes staging, queued and currently written buffers.
// It is not a heap-byte quota. Mutex/deque operations are not lock-free.
class JournalBatchQueue {
public:
    struct Batch {
        std::vector<JournalRecord> records;
        size_t slots = 0;
    };
private:
    const size_t capacity_;
    size_t used_ = 0;
    bool closed_ = false;
    std::deque<Batch> batches_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
public:
    explicit JournalBatchQueue(size_t capacity) : capacity_(capacity) {
        if (capacity == 0) throw std::invalid_argument("Journal record budget must be positive");
    }
    bool try_reserve(size_t slots) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || slots == 0 || slots > capacity_ - used_) return false;
        used_ += slots;
        return true;
    }
    void release(size_t slots) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (slots > used_) std::terminate();
        used_ -= slots;
    }
    // Reservation ownership transfers with the vector, including its capacity.
    bool try_push(std::vector<JournalRecord>&& records, size_t slots) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) return false;
            if (records.empty() || records.capacity() > slots || slots > used_) {
                throw std::logic_error("Invalid journal batch reservation");
            }
            batches_.push_back(Batch{std::move(records), slots});
        }
        not_empty_.notify_one();
        return true;
    }
    bool wait_pop(Batch& batch) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (batch.slots != 0) throw std::logic_error("Finish the previous journal batch before pop");
        not_empty_.wait(lock, [this] { return closed_ || !batches_.empty(); });
        if (batches_.empty()) return false;
        batch = std::move(batches_.front());
        batches_.pop_front();
        // Budget stays held while this allocation is on the writer thread.
        return true;
    }
    void finish(Batch& batch) noexcept {
        const size_t slots = batch.slots;
        std::vector<JournalRecord>().swap(batch.records);
        batch.slots = 0;
        release(slots);
    }
    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
    }
    size_t used_slots() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return used_;
    }
    size_t capacity() const noexcept { return capacity_; }
};

#endif
