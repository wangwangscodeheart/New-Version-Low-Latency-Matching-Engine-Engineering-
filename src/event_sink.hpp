#ifndef EVENT_SINK_HPP
#define EVENT_SINK_HPP

#include "events.hpp"
#include <array>
#include <cstddef>
#include <optional>
#include <vector>

// Sink contract: push returns false when the receiver cannot accept an event.
// OrderBook reports that condition explicitly through ProcessResult instead of
// silently dropping output.
class NullEventSink {
public:
    bool push(const EngineEvent&) noexcept { return true; }
};

class VectorEventSink {
    std::vector<EngineEvent> events_;

public:
    explicit VectorEventSink(size_t reserve = 0) { events_.reserve(reserve); }
    bool push(const EngineEvent& event) {
        events_.push_back(event);
        return true;
    }
    const std::vector<EngineEvent>& events() const noexcept { return events_; }
    std::vector<EngineEvent>& events() noexcept { return events_; }
    void clear() noexcept { events_.clear(); }
};

template<size_t Capacity>
class FixedEventBuffer {
    std::array<std::optional<EngineEvent>, Capacity> events_{};
    size_t size_ = 0;

public:
    bool push(const EngineEvent& event) noexcept {
        if (size_ == Capacity) return false;
        events_[size_++].emplace(event);
        return true;
    }
    size_t size() const noexcept { return size_; }
    const EngineEvent& operator[](size_t index) const noexcept {
        return *events_[index];
    }
    void clear() noexcept {
        for (size_t i = 0; i < size_; ++i) events_[i].reset();
        size_ = 0;
    }
};

#endif
