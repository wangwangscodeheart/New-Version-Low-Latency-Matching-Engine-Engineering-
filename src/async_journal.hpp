#ifndef ASYNC_JOURNAL_HPP
#define ASYNC_JOURNAL_HPP

#include "command_journal.hpp"
#include "journal_batch_queue.hpp"
#include <atomic>
#include <fstream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

enum class JournalHealth : uint8_t {
    HEALTHY = 0,
    QUEUE_OVERFLOW = 1,
    WRITE_FAILURE = 2,
    STOPPED = 3,
    PROTOCOL_ERROR = 4,
    ALLOCATION_FAILURE = 5
};

// Asynchronous append-only journal writer for one process session. It is not a
// claim of fsync-level crash durability: healthy()==true proves that every
// accepted in-process record was written and flushed during an orderly stop.
// A single producer owns the command/observer methods; stop requires that
// producer to be quiescent. The worker alone owns the stream after construction.
// The constructor's capacity is a RECORD SLOT budget, not a command count.
class AsyncJournalWriter : public CommandJournal {
    enum class Mode : uint8_t { NONE, CANONICAL, OBSERVER };

    JournalBatchQueue queue_;
    std::ofstream file_;
    std::thread worker_;
    EventDispatcher::Subscription subscription_;
    std::atomic<uint64_t> accepted_{0};
    std::atomic<uint64_t> written_{0};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> accepted_commands_{0};
    std::atomic<uint64_t> written_commands_{0};
    std::atomic<JournalHealth> health_{JournalHealth::HEALTHY};
    std::atomic<bool> stopped_{false};
    Mode mode_ = Mode::NONE;
    uint64_t next_sequence_ = 1;
    bool sequence_exhausted_ = false;
    bool have_timestamp_ = false;
    Timestamp last_timestamp_{0};
    std::vector<JournalRecord> staged_;
    size_t staged_slots_ = 0;
    size_t staged_limit_ = 0;
    bool active_ = false;

    void discard_stage() noexcept {
        // Free the allocation before making its reservation available again.
        std::vector<JournalRecord>().swap(staged_);
        const size_t slots = staged_slots_;
        staged_slots_ = 0;
        staged_limit_ = 0;
        active_ = false;
        if (slots != 0) queue_.release(slots);
    }

    bool refuse(JournalHealth failure) noexcept {
        JournalHealth expected = JournalHealth::HEALTHY;
        if (health_.compare_exchange_strong(expected, failure,
                                            std::memory_order_acq_rel)) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
        }
        discard_stage();
        return false;
    }

    void write_failure() noexcept {
        // An actual stream failure must remain visible even if the producer
        // already detected a different terminal failure.
        health_.store(JournalHealth::WRITE_FAILURE, std::memory_order_release);
    }

    bool sequence_room(size_t records) const noexcept {
        return !sequence_exhausted_ && records != 0 &&
            records - 1 <= std::numeric_limits<uint64_t>::max() - next_sequence_;
    }

    static bool serializable(const Symbol& symbol, const Command& command) noexcept {
        if (symbol.value().empty() ||
            symbol.value().find_first_of(",\r\n") != std::string::npos ||
            command.valueless_by_exception()) return false;
        if (const auto* order = std::get_if<NewOrderCommand>(&command)) {
            return (order->side == Side::BUY || order->side == Side::SELL) &&
                (order->time_in_force == TimeInForce::GTC ||
                 order->time_in_force == TimeInForce::IOC ||
                 order->time_in_force == TimeInForce::POST_ONLY);
        }
        return true;
    }

    bool begin(Timestamp timestamp, const Symbol& symbol, const Command& command,
               size_t max_records) {
        if (!available()) return false;
        if (active_ || max_records < 2 || !serializable(symbol, command) ||
            (have_timestamp_ && timestamp < last_timestamp_) ||
            !sequence_room(max_records)) return refuse(JournalHealth::PROTOCOL_ERROR);
        if (!queue_.try_reserve(max_records)) return refuse(JournalHealth::QUEUE_OVERFLOW);
        staged_slots_ = max_records;
        staged_limit_ = max_records;
        active_ = true;
        try {
            // Always start with a fresh allocation. Committing a small command
            // must not leave an earlier giant allocation on the producer.
            staged_.reserve(max_records);
            if (staged_.capacity() > staged_slots_) {
                const size_t extra = staged_.capacity() - staged_slots_;
                if (!queue_.try_reserve(extra)) return refuse(JournalHealth::QUEUE_OVERFLOW);
                staged_slots_ += extra;
            }
            auto input = JournalProjector::project(
                SystemEvent::order(timestamp, symbol, command), next_sequence_);
            if (!input) return refuse(JournalHealth::PROTOCOL_ERROR);
            input->format_version = 2;
            staged_.push_back(std::move(*input));
            // The worker may have failed during allocation/projection.
            if (!available()) { discard_stage(); return false; }
            return true;
        } catch (const std::bad_alloc&) {
            return refuse(JournalHealth::ALLOCATION_FAILURE);
        } catch (...) {
            return refuse(JournalHealth::PROTOCOL_ERROR);
        }
    }

    bool observer_room(size_t records) {
        if (!sequence_room(records)) return refuse(JournalHealth::PROTOCOL_ERROR);
        if (records <= staged_limit_) return true;
        if (records > queue_.capacity()) return refuse(JournalHealth::QUEUE_OVERFLOW);
        // Unlike canonical mode, observer mode learns its bound after matching.
        // Charge the new capacity before growing, and retain it through writing.
        if (records > staged_slots_) {
            const size_t extra = records - staged_slots_;
            if (!queue_.try_reserve(extra)) return refuse(JournalHealth::QUEUE_OVERFLOW);
            staged_slots_ += extra;
        }
        try {
            staged_.reserve(records);
            if (staged_.capacity() > staged_slots_) {
                const size_t extra = staged_.capacity() - staged_slots_;
                if (!queue_.try_reserve(extra)) return refuse(JournalHealth::QUEUE_OVERFLOW);
                staged_slots_ += extra;
            }
            staged_limit_ = records;
            return true;
        } catch (const std::bad_alloc&) {
            return refuse(JournalHealth::ALLOCATION_FAILURE);
        } catch (...) {
            return refuse(JournalHealth::PROTOCOL_ERROR);
        }
    }

    bool append(const TradeEvent& trade) {
        if (!available()) { discard_stage(); return false; }
        if (!active_ || staged_.empty()) return refuse(JournalHealth::PROTOCOL_ERROR);
        const JournalRecord& input = staged_.front();
        if (input.type != JournalRecordType::ORDER_INSERT ||
            trade.type != EventType::TRADE ||
            trade.command_sequence != input.command_sequence ||
            trade.aggressive_order_id != input.order_id) {
            return refuse(JournalHealth::PROTOCOL_ERROR);
        }
        // Always leave one record for COMMAND_RESULT; canonical mode never
        // grows its allocation after begin_command has returned.
        if (staged_.size() > std::numeric_limits<size_t>::max() - 2) {
            return refuse(JournalHealth::PROTOCOL_ERROR);
        }
        const size_t required = staged_.size() + 2;
        if (mode_ == Mode::OBSERVER) {
            if (!observer_room(required)) return false;
        } else if (required > staged_limit_) {
            return refuse(JournalHealth::PROTOCOL_ERROR);
        }
        try {
            // observer_room may reallocate staged_, so reacquire the context.
            const JournalRecord& context = staged_.front();
            auto record = JournalProjector::project(SystemEvent::engine(
                context.timestamp, context.symbol, EngineEvent{trade}),
                next_sequence_ + static_cast<uint64_t>(staged_.size()));
            if (!record) return refuse(JournalHealth::PROTOCOL_ERROR);
            record->format_version = 2;
            staged_.push_back(std::move(*record));
            if (!available()) { discard_stage(); return false; }
            return true;
        } catch (const std::bad_alloc&) {
            return refuse(JournalHealth::ALLOCATION_FAILURE);
        } catch (...) {
            return refuse(JournalHealth::PROTOCOL_ERROR);
        }
    }

    bool commit(CommandOutcome outcome) {
        if (!available()) { discard_stage(); return false; }
        if (!active_ || staged_.empty() || staged_.size() >= staged_limit_ ||
            static_cast<unsigned>(outcome) >=
                static_cast<unsigned>(CommandOutcome::SYSTEM_UNAVAILABLE) ||
            (staged_.size() > 1 && outcome != CommandOutcome::APPLIED)) {
            return refuse(JournalHealth::PROTOCOL_ERROR);
        }
        try {
            const JournalRecord& input = staged_.front();
            const Timestamp timestamp = input.timestamp;
            auto result = JournalProjector::project(SystemEvent::processing_latency(
                input.timestamp, input.symbol, ProcessingLatencyEvent{
                    0, outcome, input.command_sequence, input.order_id}),
                next_sequence_ + static_cast<uint64_t>(staged_.size()));
            if (!result) return refuse(JournalHealth::PROTOCOL_ERROR);
            result->format_version = 2;
            staged_.push_back(std::move(*result));
            if (!available()) { discard_stage(); return false; }
            const size_t count = staged_.size();
            // Exactly one queue operation transfers the entire group and its
            // full worst-case reservation; nothing is accepted on failure.
            if (!queue_.try_push(std::move(staged_), staged_slots_)) {
                return refuse(JournalHealth::PROTOCOL_ERROR);
            }
            staged_slots_ = 0;
            staged_limit_ = 0;
            active_ = false;
            std::vector<JournalRecord>().swap(staged_);
            if (count - 1 == std::numeric_limits<uint64_t>::max() - next_sequence_) {
                sequence_exhausted_ = true;
            } else {
                next_sequence_ += static_cast<uint64_t>(count);
            }
            last_timestamp_ = timestamp;
            have_timestamp_ = true;
            accepted_.fetch_add(static_cast<uint64_t>(count), std::memory_order_relaxed);
            accepted_commands_.fetch_add(1, std::memory_order_relaxed);
            return true;
        } catch (const std::bad_alloc&) {
            return refuse(JournalHealth::ALLOCATION_FAILURE);
        } catch (...) {
            return refuse(JournalHealth::PROTOCOL_ERROR);
        }
    }

    void run() noexcept {
        JournalBatchQueue::Batch batch;
        bool stream_failed = false;
        try {
            while (queue_.wait_pop(batch)) {
                bool complete = !stream_failed;
                if (!stream_failed) {
                    try {
                        for (const JournalRecord& record : batch.records) {
                            write_journal_record(file_, record);
                            if (!file_) { complete = false; break; }
                            written_.fetch_add(1, std::memory_order_relaxed);
                        }
                    } catch (...) {
                        complete = false;
                    }
                    if (!complete) {
                        stream_failed = true;
                        write_failure();
                    }
                }
                if (complete) written_commands_.fetch_add(1, std::memory_order_relaxed);
                // Even failed batches must release their allocation and budget.
                queue_.finish(batch);
            }
        } catch (...) {
            write_failure();
            if (batch.slots != 0) queue_.finish(batch);
        }
        try {
            file_.flush();
            if (!file_) write_failure();
        } catch (...) {
            write_failure();
        }
        try {
            file_.close();
            if (file_.fail()) write_failure();
        } catch (...) {
            write_failure();
        }
    }

    void on_event(const SystemEvent& event) {
        if (event.event_type != SystemEventType::ORDER &&
            event.event_type != SystemEventType::TRADE &&
            event.event_type != SystemEventType::PROCESSING_LATENCY) return;
        if (!available() || mode_ != Mode::OBSERVER) {
            throw std::runtime_error("Async journal observer is unavailable");
        }
        bool accepted = false;
        try {
            if (event.event_type == SystemEventType::ORDER) {
                accepted = begin(event.timestamp, event.symbol,
                    std::get<OrderEvent>(event.payload).command, 2);
            } else if (!active_ || staged_.empty() ||
                       event.timestamp != staged_.front().timestamp ||
                       event.symbol != staged_.front().symbol) {
                accepted = refuse(JournalHealth::PROTOCOL_ERROR);
            } else if (event.event_type == SystemEventType::TRADE) {
                accepted = append(std::get<TradeEvent>(std::get<EngineEvent>(event.payload)));
            } else {
                const auto& result = std::get<ProcessingLatencyEvent>(event.payload);
                if (result.command_sequence != staged_.front().command_sequence ||
                    result.order_id != staged_.front().order_id) {
                    accepted = refuse(JournalHealth::PROTOCOL_ERROR);
                } else {
                    accepted = commit(result.outcome);
                }
            }
        } catch (const std::bad_alloc&) {
            refuse(JournalHealth::ALLOCATION_FAILURE);
        } catch (...) {
            refuse(JournalHealth::PROTOCOL_ERROR);
        }
        // Observers have no boolean acknowledgment channel. Propagate refusal
        // so TradingEngine retains undelivered events and fails closed.
        if (!accepted) throw std::runtime_error("Async journal observer refused a command group");
    }

public:
    AsyncJournalWriter(const std::string& filename, size_t queue_capacity)
        : queue_(queue_capacity), file_(filename, std::ios::out | std::ios::trunc) {
        if (!file_) throw std::runtime_error("Cannot open async journal: " + filename);
        write_command_journal_header(file_);
        file_.flush();
        if (!file_) throw std::runtime_error("Cannot write async journal header: " + filename);
        worker_ = std::thread([this] { run(); });
    }

    AsyncJournalWriter(const AsyncJournalWriter&) = delete;
    AsyncJournalWriter& operator=(const AsyncJournalWriter&) = delete;
    ~AsyncJournalWriter() { stop(); }

    void attach(EventDispatcher& dispatcher) {
        if (!available() || mode_ != Mode::NONE || active_) {
            refuse(JournalHealth::PROTOCOL_ERROR);
            throw std::logic_error("Async journal canonical and observer modes are exclusive");
        }
        try {
            subscription_ = dispatcher.subscribe_all(
                [this](const SystemEvent& event) { on_event(event); });
            mode_ = Mode::OBSERVER;
        } catch (const std::bad_alloc&) {
            refuse(JournalHealth::ALLOCATION_FAILURE);
            throw;
        } catch (...) {
            refuse(JournalHealth::PROTOCOL_ERROR);
            throw;
        }
    }

    bool begin_command(Timestamp timestamp, const Symbol& symbol,
                       const Command& command, size_t max_records) override {
        if (!available()) return false;
        if (mode_ == Mode::OBSERVER) return refuse(JournalHealth::PROTOCOL_ERROR);
        mode_ = Mode::CANONICAL;
        return begin(timestamp, symbol, command, max_records);
    }

    bool append_trade(const TradeEvent& trade) override {
        if (mode_ != Mode::CANONICAL) return refuse(JournalHealth::PROTOCOL_ERROR);
        return append(trade);
    }

    bool commit_command(CommandOutcome outcome) override {
        if (mode_ != Mode::CANONICAL) return refuse(JournalHealth::PROTOCOL_ERROR);
        return commit(outcome);
    }

    void abort_command() noexcept override {
        if (active_) refuse(JournalHealth::PROTOCOL_ERROR);
    }

    void stop() noexcept {
        bool expected = false;
        if (!stopped_.compare_exchange_strong(expected, true,
                                               std::memory_order_acq_rel)) return;
        abort_command();
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
               accepted_count() == written_count() &&
               accepted_command_count() == written_command_count();
    }
    bool available() const noexcept override {
        return health() == JournalHealth::HEALTHY &&
               !stopped_.load(std::memory_order_acquire);
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
    uint64_t accepted_command_count() const noexcept {
        return accepted_commands_.load(std::memory_order_relaxed);
    }
    uint64_t written_command_count() const noexcept {
        return written_commands_.load(std::memory_order_relaxed);
    }
    size_t buffered_record_slots() const { return queue_.used_slots(); }
    size_t record_slot_capacity() const noexcept { return queue_.capacity(); }
};

#endif
