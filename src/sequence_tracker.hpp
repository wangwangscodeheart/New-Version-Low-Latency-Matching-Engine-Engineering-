#ifndef SEQUENCE_TRACKER_HPP
#define SEQUENCE_TRACKER_HPP

#include "instrument_id.hpp"
#include <cstdint>
#include <unordered_map>

enum class FeedSequenceStatus : uint8_t { FIRST, IN_ORDER, GAP, DUPLICATE, OUT_OF_ORDER };

class SequenceTracker {
    std::unordered_map<uint32_t, uint64_t> last_;
public:
    FeedSequenceStatus observe(InstrumentId instrument, uint64_t sequence) {
        const auto found = last_.find(instrument.get());
        if (found == last_.end()) {
            last_.emplace(instrument.get(), sequence);
            return FeedSequenceStatus::FIRST;
        }
        if (sequence == found->second) return FeedSequenceStatus::DUPLICATE;
        if (sequence < found->second) return FeedSequenceStatus::OUT_OF_ORDER;
        if (sequence != found->second + 1) return FeedSequenceStatus::GAP;
        found->second = sequence;
        return FeedSequenceStatus::IN_ORDER;
    }
    uint64_t last(InstrumentId instrument) const noexcept {
        const auto found = last_.find(instrument.get());
        return found == last_.end() ? 0 : found->second;
    }
    void restore(InstrumentId instrument, uint64_t sequence) {
        last_[instrument.get()] = sequence;
    }
};

#endif
