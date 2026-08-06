#ifndef INSTRUMENT_ID_HPP
#define INSTRUMENT_ID_HPP

#include <cstdint>
#include <limits>

class InstrumentId {
    uint32_t value_;
public:
    static constexpr uint32_t INVALID_VALUE = std::numeric_limits<uint32_t>::max();
    explicit constexpr InstrumentId(uint32_t value = INVALID_VALUE) noexcept : value_(value) {}
    constexpr uint32_t get() const noexcept { return value_; }
    constexpr bool valid() const noexcept { return value_ != INVALID_VALUE; }
    constexpr bool operator==(InstrumentId other) const noexcept { return value_ == other.value_; }
    constexpr bool operator!=(InstrumentId other) const noexcept { return !(*this == other); }
};

#endif
