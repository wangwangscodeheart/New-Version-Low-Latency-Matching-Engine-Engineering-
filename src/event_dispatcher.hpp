#ifndef EVENT_DISPATCHER_HPP
#define EVENT_DISPATCHER_HPP

#include "system_events.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>
#include <stdexcept>

class EventDispatcher {
public:
    using Handler = std::function<void(const SystemEvent&)>;

private:
    static constexpr size_t TYPE_COUNT = 8;
    struct HandlerEntry {
        uint64_t id;
        Handler handler;
        bool active = true;
    };
    struct State {
        std::array<std::vector<HandlerEntry>, TYPE_COUNT> handlers;
        std::vector<HandlerEntry> all_handlers;
        uint64_t next_id = 1;
        bool frozen = false;

        void unsubscribe(uint64_t id) {
            if (frozen) {
                auto deactivate = [id](auto& entries) {
                    for (HandlerEntry& entry : entries) {
                        if (entry.id == id) entry.active = false;
                    }
                };
                deactivate(all_handlers);
                for (auto& entries : handlers) deactivate(entries);
                return;
            }
            auto remove = [id](auto& entries) {
                entries.erase(std::remove_if(entries.begin(), entries.end(),
                    [id](const HandlerEntry& entry) { return entry.id == id; }),
                    entries.end());
            };
            remove(all_handlers);
            for (auto& entries : handlers) remove(entries);
        }
    };

public:
    class Subscription {
        std::weak_ptr<State> state_;
        uint64_t id_ = 0;

        Subscription(const std::shared_ptr<State>& state, uint64_t id)
            : state_(state), id_(id) {}
        friend class EventDispatcher;

    public:
        Subscription() = default;
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;

        Subscription(Subscription&& other) noexcept
            : state_(std::move(other.state_)), id_(other.id_) {
            other.id_ = 0;
        }

        Subscription& operator=(Subscription&& other) noexcept {
            if (this != &other) {
                reset();
                state_ = std::move(other.state_);
                id_ = other.id_;
                other.id_ = 0;
            }
            return *this;
        }

        ~Subscription() { reset(); }

        void reset() noexcept {
            if (id_ != 0) {
                if (const auto state = state_.lock()) state->unsubscribe(id_);
                id_ = 0;
                state_.reset();
            }
        }

        bool active() const noexcept { return id_ != 0 && !state_.expired(); }
    };

private:
    std::shared_ptr<State> state_ = std::make_shared<State>();

    static constexpr size_t index(SystemEventType type) noexcept {
        return static_cast<size_t>(type);
    }

public:
    EventDispatcher() = default;
    EventDispatcher(const EventDispatcher&) = delete;
    EventDispatcher& operator=(const EventDispatcher&) = delete;

    // Subscription order is dispatch order. Mutable mode snapshots callbacks.
    // After freeze(), publish iterates stable storage directly and allocates no
    // callback vector; resetting a token merely marks its entry inactive.
    Subscription subscribe(SystemEventType type, Handler handler) {
        if (state_->frozen) throw std::logic_error("EventDispatcher is frozen");
        const uint64_t id = state_->next_id++;
        state_->handlers[index(type)].push_back(HandlerEntry{id, std::move(handler), true});
        return Subscription(state_, id);
    }

    Subscription subscribe_all(Handler handler) {
        if (state_->frozen) throw std::logic_error("EventDispatcher is frozen");
        const uint64_t id = state_->next_id++;
        state_->all_handlers.push_back(HandlerEntry{id, std::move(handler), true});
        return Subscription(state_, id);
    }

    void publish(const SystemEvent& event) const {
        if (state_->frozen) {
            for (const HandlerEntry& entry : state_->all_handlers) {
                if (entry.active) entry.handler(event);
            }
            for (const HandlerEntry& entry : state_->handlers[index(event.event_type)]) {
                if (entry.active) entry.handler(event);
            }
            return;
        }
        std::vector<Handler> callbacks;
        callbacks.reserve(state_->all_handlers.size() +
                          state_->handlers[index(event.event_type)].size());
        for (const HandlerEntry& entry : state_->all_handlers) {
            if (entry.active) callbacks.push_back(entry.handler);
        }
        for (const HandlerEntry& entry : state_->handlers[index(event.event_type)]) {
            if (entry.active) callbacks.push_back(entry.handler);
        }
        for (const Handler& callback : callbacks) callback(event);
    }

    void freeze() noexcept { state_->frozen = true; }
    bool frozen() const noexcept { return state_->frozen; }

    size_t subscriber_count(SystemEventType type) const noexcept {
        return static_cast<size_t>(std::count_if(
            state_->handlers[index(type)].begin(), state_->handlers[index(type)].end(),
            [](const HandlerEntry& entry) { return entry.active; }));
    }

    size_t all_event_subscriber_count() const noexcept {
        return static_cast<size_t>(std::count_if(
            state_->all_handlers.begin(), state_->all_handlers.end(),
            [](const HandlerEntry& entry) { return entry.active; }));
    }
};

#endif
