#ifndef MARKET_DATA_GATEWAY_HPP
#define MARKET_DATA_GATEWAY_HPP

#include "market_data_book.hpp"
#include "sequence_tracker.hpp"
#include <cstddef>
#include <vector>

struct FeedGatewayMetrics {
    uint64_t applied = 0;
    uint64_t gaps = 0;
    uint64_t duplicates = 0;
    uint64_t out_of_order = 0;
    uint64_t invalid_instruments = 0;
    uint64_t invalid_messages = 0;
};

struct MarketDataCheckpoint {
    std::vector<MarketDataBookState> books;
    std::vector<uint64_t> sequences;
    FeedGatewayMetrics metrics;
};

class MarketDataGateway {
    std::vector<MarketDataBook> books_;
    SequenceTracker sequences_;
    FeedGatewayMetrics metrics_;
public:
    explicit MarketDataGateway(size_t instrument_count) : books_(instrument_count) {}

    bool on_message(const FeedMessage& message) {
        const InstrumentId id = message.header.instrument_id;
        if (!id.valid() || id.get() >= books_.size()) {
            ++metrics_.invalid_instruments;
            return false;
        }
        const FeedSequenceStatus status = sequences_.observe(id, message.header.sequence);
        if (status == FeedSequenceStatus::DUPLICATE) {
            ++metrics_.duplicates;
            return false;
        }
        if (status == FeedSequenceStatus::OUT_OF_ORDER) {
            ++metrics_.out_of_order;
            return false;
        }
        if (status == FeedSequenceStatus::GAP) ++metrics_.gaps;
        if (!books_[id.get()].apply(message.payload)) {
            ++metrics_.invalid_messages;
            return false;
        }
        ++metrics_.applied;
        return true;
    }

    void restore_sequence(InstrumentId id, uint64_t sequence) {
        if (!id.valid() || id.get() >= books_.size()) {
            throw std::out_of_range("invalid feed checkpoint instrument");
        }
        sequences_.restore(id, sequence);
    }
    MarketDataCheckpoint checkpoint() const {
        MarketDataCheckpoint result;
        result.books.reserve(books_.size());
        result.sequences.reserve(books_.size());
        for (size_t i = 0; i < books_.size(); ++i) {
            result.books.push_back(books_[i].capture_state());
            result.sequences.push_back(sequences_.last(
                InstrumentId(static_cast<uint32_t>(i))));
        }
        result.metrics = metrics_;
        return result;
    }
    void restore_checkpoint(const MarketDataCheckpoint& checkpoint) {
        if (checkpoint.books.size() != books_.size() ||
            checkpoint.sequences.size() != books_.size()) {
            throw std::runtime_error("market data checkpoint instrument mismatch");
        }
        std::vector<MarketDataBook> restored_books;
        restored_books.reserve(books_.size());
        SequenceTracker restored_sequences;
        for (size_t i = 0; i < books_.size(); ++i) {
            restored_books.push_back(MarketDataBook::restore(checkpoint.books[i]));
            restored_sequences.restore(InstrumentId(static_cast<uint32_t>(i)),
                                       checkpoint.sequences[i]);
        }
        books_ = std::move(restored_books);
        sequences_ = std::move(restored_sequences);
        metrics_ = checkpoint.metrics;
    }
    const MarketDataBook* book(InstrumentId id) const noexcept {
        return id.valid() && id.get() < books_.size() ? &books_[id.get()] : nullptr;
    }
    uint64_t last_sequence(InstrumentId id) const noexcept { return sequences_.last(id); }
    const FeedGatewayMetrics& metrics() const noexcept { return metrics_; }
};

#endif
