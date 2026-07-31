#ifndef BOUNDED_QUEUE_HPP
#define BOUNDED_QUEUE_HPP

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>

// Bounded MPSC-safe queue with a non-blocking producer path. The current
// trading architecture has one producer, but this implementation also keeps
// metrics/log-control callers safe without changing the public contract.
template<typename T>
class BoundedQueue {
    const size_t capacity_;
    std::deque<T> queue_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    bool closed_ = false;

public:
    explicit BoundedQueue(size_t capacity) : capacity_(capacity) {
        if (capacity == 0) throw std::invalid_argument("Queue capacity must be positive");
    }

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    bool try_push(T value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_ || queue_.size() >= capacity_) return false;
            queue_.push_back(std::move(value));
        }
        not_empty_.notify_one();
        return true;
    }

    // Returns false only after close() and after every queued item is drained.
    bool wait_pop(T& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) return false;
        value = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    size_t capacity() const noexcept { return capacity_; }
};

#endif
