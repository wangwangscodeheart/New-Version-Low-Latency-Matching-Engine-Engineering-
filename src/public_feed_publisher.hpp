#ifndef PUBLIC_FEED_PUBLISHER_HPP
#define PUBLIC_FEED_PUBLISHER_HPP

#include "committed_command_sink.hpp"
#include "feed_encoder.hpp"
#include "market_data_gateway.hpp"
#include "market_manager.hpp"
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

// Each consume is one COMPLETE command batch, not a stream fragment. A false
// return may mean a prefix was consumed; callers must invalidate, never retry.
class FeedByteSink {
public:
    virtual ~FeedByteSink() = default;
    virtual bool consume(const uint8_t* bytes, size_t size) = 0;
    virtual void invalidate() noexcept = 0;
};

// An intentionally terminal adapter: even a partially applied failed batch
// immediately makes every consumer book STALE. No automatic recovery/retry.
class GatewayFeedSink final : public FeedByteSink {
    MarketDataGateway& gateway_;
    FeedDecoder decoder_;
    bool healthy_ = true;
public:
    explicit GatewayFeedSink(MarketDataGateway& gateway) : gateway_(gateway) {
        try {
            auto anchor = gateway_.checkpoint();
            for (size_t i = 0; i < anchor.books.size(); ++i) {
                if (!anchor.books[i].orders.empty() || anchor.sequences[i] != 0 ||
                    anchor.states[i] != FeedState::HEALTHY) {
                    throw std::invalid_argument("Public feed requires an empty healthy consumer at sequence zero");
                }
            }
            // SequenceTracker otherwise accepts ANY first sequence. Restoring
            // explicit zero anchors makes the first expected sequence one.
            gateway_.restore_checkpoint(anchor);
        } catch (...) {
            invalidate();
            throw;
        }
    }
    bool consume(const uint8_t* bytes, size_t size) noexcept override {
        if (!healthy_) return false;
        try {
            if (size != 0 && !bytes) throw std::invalid_argument("Null feed batch");
            if (size != 0) decoder_.consume(bytes, size, [this](const FeedMessage& message) {
                if (!gateway_.on_message(message)) {
                    throw std::runtime_error("Public feed consumer rejected a message");
                }
            });
            decoder_.finish();
            return true;
        } catch (...) {
            invalidate();
            return false;
        }
    }
    void invalidate() noexcept override {
        healthy_ = false;
        gateway_.invalidate_all();
        decoder_ = FeedDecoder{};
    }
    bool available() const noexcept { return healthy_; }
    size_t pending_bytes() const noexcept { return decoder_.pending_bytes(); }
    size_t instrument_count() const noexcept { return gateway_.instrument_count(); }
};

// Single-writer in-process projection after CommandJournal acceptance. Public
// references/sequences are a fresh session namespace, never private order IDs.
// byte_budget bounds retained encoded payloads INCLUDING the current drain
// call, not total heap usage (maps, deque nodes, and decoder storage are extra).
// No I/O, worker thread, durability, resynchronization, or public FIFO promise.
class PublicFeedPublisher final : public CommittedCommandSink {
    struct PublicOrder {
        uint64_t reference;
        Price price;
        Quantity remaining;
        Side side;
        PrioritySequence priority;
    };
    struct Instrument {
        const OrderBook* book;
        Symbol symbol;
        InstrumentConfig rules;
        PriceLadderConfig ladder;
        uint64_t last_command;
        uint64_t last_reference = 0;
        uint64_t last_sequence = 0;
        std::unordered_map<uint64_t, PublicOrder> orders;
    };
    struct Batch {
        std::unique_ptr<uint8_t[]> bytes;
        size_t size;
    };
    MarketManager& markets_;
    FeedByteSink& sink_;
    const size_t byte_budget_;
    std::vector<Instrument> instruments_;
    std::deque<Batch> batches_;
    size_t pending_bytes_ = 0;
    size_t in_flight_bytes_ = 0;
    bool healthy_ = true;
    bool draining_ = false;

    static void require(bool condition) {
        if (!condition) throw std::runtime_error("Invalid committed public feed context");
    }
    bool source_configuration_valid() const noexcept {
        if (markets_.symbol_count() != instruments_.size()) return false;
        for (size_t i = 0; i < instruments_.size(); ++i) {
            const auto& instrument = instruments_[i];
            const InstrumentId id(static_cast<uint32_t>(i));
            const OrderBook* book = markets_.find_book(id);
            const Symbol* symbol = markets_.symbol(id);
            if (book != instrument.book || !symbol || *symbol != instrument.symbol ||
                !book->event_recording_enabled() ||
                !(book->instrument_config() == instrument.rules) ||
                !(book->price_ladder_config() == instrument.ladder)) return false;
        }
        return true;
    }
    static uint64_t increment(uint64_t& value) {
        require(value != std::numeric_limits<uint64_t>::max());
        return ++value;
    }
    void encode(Instrument& instrument, InstrumentId id, Timestamp timestamp,
                FeedMessageType type, FeedPayload payload, uint8_t* bytes,
                size_t& offset) {
        const uint64_t sequence = increment(instrument.last_sequence);
        const uint16_t body_size = type == FeedMessageType::ADD_ORDER ? 25 :
            type == FeedMessageType::CANCEL_ORDER ? 8 : 32;
        if (auto* trade = std::get_if<FeedTrade>(&payload)) trade->trade_id = sequence;
        const auto frame = FeedEncoder::encode(FeedMessage{
            {type, body_size, id, sequence, timestamp}, std::move(payload)});
        std::memcpy(bytes + offset, frame.data(), frame.size());
        offset += frame.size();
    }
    static void validate_rejection(const Command& command,
                                   const std::vector<EngineEvent>& events) {
        require(events.size() == 1);
        const auto* rejection = std::get_if<OrderRejectedEvent>(&events.front());
        require(rejection && rejection->type == EventType::ORDER_REJECTED &&
            rejection->command_type == get_command_type(command) &&
            rejection->order_id == get_order_id(command) &&
            static_cast<uint8_t>(rejection->reason) <=
                static_cast<uint8_t>(RejectReason::INVALID_TIME_IN_FORCE));
    }
    void project_new(Instrument& instrument, InstrumentId id, Timestamp timestamp,
                     const NewOrderCommand& command,
                     const std::vector<EngineEvent>& events,
                     uint8_t* bytes, size_t& offset) {
        require(command.order_id.get() != 0 && command.price.get() > 0 &&
            command.quantity.get() != 0 &&
            (command.side == Side::BUY || command.side == Side::SELL) &&
            (command.time_in_force == TimeInForce::GTC ||
             command.time_in_force == TimeInForce::IOC ||
             command.time_in_force == TimeInForce::POST_ONLY) &&
            instrument.orders.find(command.order_id.get()) == instrument.orders.end());
        uint64_t remaining = command.quantity.get();
        bool terminal_event = false;
        for (const EngineEvent& event : events) {
            require(!terminal_event);
            if (const auto* trade = std::get_if<TradeEvent>(&event)) {
                require(trade->type == EventType::TRADE &&
                    command.time_in_force != TimeInForce::POST_ONLY &&
                    trade->aggressive_order_id == command.order_id &&
                    trade->quantity.get() != 0 && trade->quantity.get() <= remaining);
                const auto passive = instrument.orders.find(trade->passive_order_id.get());
                require(passive != instrument.orders.end());
                PublicOrder& order = passive->second;
                require(order.side != command.side && order.price == trade->price &&
                    trade->quantity.get() <= order.remaining.get() &&
                    (command.side == Side::BUY ? trade->price <= command.price :
                                                trade->price >= command.price));
                encode(instrument, id, timestamp, FeedMessageType::TRADE,
                    FeedTrade{0, order.reference, trade->price, trade->quantity}, bytes, offset);
                remaining -= trade->quantity.get();
                order.remaining = Quantity(order.remaining.get() - trade->quantity.get());
                if (order.remaining.get() == 0) instrument.orders.erase(passive);
            } else if (const auto* rested = std::get_if<OrderRestedEvent>(&event)) {
                require(rested->type == EventType::ORDER_RESTED && remaining != 0 &&
                    command.time_in_force != TimeInForce::IOC &&
                    rested->order_id == command.order_id && rested->side == command.side &&
                    rested->price == command.price && rested->original_quantity == command.quantity &&
                    rested->remaining_quantity.get() == remaining &&
                    rested->priority_sequence.get() == command.command_sequence.get());
                const uint64_t reference = increment(instrument.last_reference);
                require(instrument.orders.emplace(command.order_id.get(), PublicOrder{
                    reference, rested->price, rested->remaining_quantity, rested->side,
                    rested->priority_sequence}).second);
                encode(instrument, id, timestamp, FeedMessageType::ADD_ORDER,
                    FeedAddOrder{reference, rested->price, rested->remaining_quantity,
                                 rested->side}, bytes, offset);
                terminal_event = true;
            } else if (const auto* cancelled = std::get_if<OrderCancelledEvent>(&event)) {
                // IOC residuals never entered the public book. In particular,
                // do not manufacture a Cancel for their private aggressor ID.
                require(cancelled->type == EventType::ORDER_CANCELLED && remaining != 0 &&
                    command.time_in_force == TimeInForce::IOC &&
                    cancelled->order_id == command.order_id && cancelled->side == command.side &&
                    cancelled->price == command.price && cancelled->cancelled_quantity.get() == remaining &&
                    cancelled->priority_sequence.get() == command.command_sequence.get());
                terminal_event = true;
            } else {
                require(false);
            }
        }
        require(remaining == 0 ? !terminal_event : terminal_event);
    }
    void project_cancel(Instrument& instrument, InstrumentId id, Timestamp timestamp,
                        const CancelOrderCommand& command,
                        const std::vector<EngineEvent>& events,
                        uint8_t* bytes, size_t& offset) {
        require(events.size() == 1);
        const auto* cancelled = std::get_if<OrderCancelledEvent>(&events.front());
        const auto found = instrument.orders.find(command.order_id.get());
        require(cancelled && found != instrument.orders.end());
        const PublicOrder& order = found->second;
        require(cancelled->type == EventType::ORDER_CANCELLED &&
            cancelled->order_id == command.order_id && cancelled->side == order.side &&
            cancelled->price == order.price && cancelled->cancelled_quantity == order.remaining &&
            cancelled->priority_sequence == order.priority);
        encode(instrument, id, timestamp, FeedMessageType::CANCEL_ORDER,
            FeedCancelOrder{order.reference}, bytes, offset);
        instrument.orders.erase(found);
    }
public:
    PublicFeedPublisher(MarketManager& markets, FeedByteSink& sink, size_t byte_budget)
        : markets_(markets), sink_(sink), byte_budget_(byte_budget) {
        try {
            instruments_.reserve(markets_.symbol_count());
            for (size_t i = 0; i < markets_.symbol_count(); ++i) {
                const InstrumentId id(static_cast<uint32_t>(i));
                const OrderBook* book = markets_.find_book(id);
                require(book && book->ready_for_commands() && book->event_recording_enabled() &&
                    book->active_order_count() == 0);
                instruments_.push_back(Instrument{book, *markets_.symbol(id),
                    book->instrument_config(), book->price_ladder_config(),
                    book->last_applied_command_sequence().get(), 0, 0, {}});
            }
        } catch (...) {
            invalidate();
            throw;
        }
    }
    bool available() const noexcept override { return healthy_ && !draining_; }
    size_t pending_bytes() const noexcept { return pending_bytes_; }
    size_t pending_batches() const noexcept {
        return batches_.size() + (in_flight_bytes_ != 0 ? 1 : 0);
    }
    bool publish(Timestamp timestamp, InstrumentId id, const Command& command,
                 CommandOutcome outcome, const std::vector<EngineEvent>& events) noexcept override {
        if (!healthy_) return false;
        if (draining_) { invalidate(); return false; }
        try {
            require(source_configuration_valid());
            if (outcome == CommandOutcome::UNKNOWN_SYMBOL) {
                require(!id.valid() && events.empty());
                return true;
            }
            require(id.valid() && id.get() < instruments_.size());
            Instrument& instrument = instruments_[id.get()];
            const uint64_t command_sequence = get_command_sequence(command).get();
            for (size_t i = 0; i < instruments_.size(); ++i) {
                const auto& source = instruments_[i];
                const uint64_t expected = i == id.get() && outcome != CommandOutcome::SEQUENCE_REJECTED
                    ? command_sequence : source.last_command;
                require(source.book->last_applied_command_sequence().get() == expected);
            }
            if (outcome == CommandOutcome::SEQUENCE_REJECTED) {
                require(events.empty() && (command_sequence == 0 || command_sequence <= instrument.last_command));
                return true;
            }
            require(command_sequence > instrument.last_command);
            for (size_t i = 0; i < events.size(); ++i) {
                require(get_command_sequence(events[i]).get() == command_sequence &&
                    get_event_index(events[i]).get() == i);
            }
            if (outcome == CommandOutcome::BUSINESS_REJECTED) {
                validate_rejection(command, events);
                require(instrument.book->active_order_count() == instrument.orders.size());
                instrument.last_command = command_sequence;
                return true;
            }
            require(outcome == CommandOutcome::APPLIED);
            size_t bytes_needed = 0;
            for (const EngineEvent& event : events) {
                const size_t frame_size = std::holds_alternative<OrderRestedEvent>(event) ? 49 :
                    std::holds_alternative<TradeEvent>(event) ? 56 :
                    std::holds_alternative<OrderCancelledEvent>(event) &&
                        std::holds_alternative<CancelOrderCommand>(command) ? 32 : 0;
                require(frame_size <= std::numeric_limits<size_t>::max() - bytes_needed);
                bytes_needed += frame_size;
            }
            require(bytes_needed <= byte_budget_ - pending_bytes_);
            std::unique_ptr<uint8_t[]> bytes;
            if (bytes_needed != 0) bytes = std::make_unique<uint8_t[]>(bytes_needed);
            size_t offset = 0;
            if (const auto* order = std::get_if<NewOrderCommand>(&command)) {
                project_new(instrument, id, timestamp, *order, events, bytes.get(), offset);
            } else {
                project_cancel(instrument, id, timestamp, std::get<CancelOrderCommand>(command),
                               events, bytes.get(), offset);
            }
            require(offset == bytes_needed &&
                instrument.book->active_order_count() == instrument.orders.size());
            if (bytes_needed != 0) {
                batches_.push_back(Batch{std::move(bytes), bytes_needed});
                pending_bytes_ += bytes_needed;
            }
            instrument.last_command = command_sequence;
            return true;
        } catch (...) {
            invalidate();
            return false;
        }
    }
    bool drain() noexcept {
        if (!healthy_) return false;
        if (draining_) { invalidate(); return false; }
        draining_ = true;
        while (healthy_ && !batches_.empty()) {
            Batch batch = std::move(batches_.front());
            batches_.pop_front();
            in_flight_bytes_ = batch.size;
            bool accepted = false;
            try { accepted = sink_.consume(batch.bytes.get(), batch.size); }
            catch (...) { /* Partial acceptance is unknown; never retry. */ }
            if (!accepted || !healthy_) invalidate();
            pending_bytes_ -= batch.size;
            in_flight_bytes_ = 0;
        }
        draining_ = false;
        return healthy_;
    }
    void invalidate() noexcept override {
        const bool notify_sink = healthy_;
        healthy_ = false;
        batches_.clear();
        pending_bytes_ = in_flight_bytes_;
        if (notify_sink) sink_.invalidate();
    }
};

#endif
