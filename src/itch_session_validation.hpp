#ifndef ITCH_SESSION_VALIDATION_HPP
#define ITCH_SESSION_VALIDATION_HPP

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace nasdaq_itch {

enum class TemporalValidationErrorCode {
    INVALID_DATE,
    OUT_OF_DAY,
    TIMESTAMP_REGRESSION,
    COUNTER_OVERFLOW
};

class TemporalValidationError : public std::runtime_error {
public:
    TemporalValidationError(TemporalValidationErrorCode code, const char* reason)
        : std::runtime_error(reason), code_(code) {}
    TemporalValidationErrorCode code() const noexcept { return code_; }
private:
    TemporalValidationErrorCode code_;
};

// An out-of-band, proleptic Gregorian calendar label. parse() validates exact
// ASCII YYYY-MM-DD and years 1900..9999. It does not establish that the venue
// traded on that day, authenticate the source, or recover a date from ITCH.
// Direct aggregate construction is a raw value; parse() is the checked entry.
struct TradingDate {
    int year;
    unsigned month;
    unsigned day;

    static TradingDate parse(std::string_view text) {
        if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
            throw TemporalValidationError(TemporalValidationErrorCode::INVALID_DATE,
                "trading date must have exact YYYY-MM-DD shape");
        }
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (i == 4 || i == 7) continue;
            if (text[i] < '0' || text[i] > '9') {
                throw TemporalValidationError(TemporalValidationErrorCode::INVALID_DATE,
                    "trading date must contain ASCII decimal digits");
            }
        }
        const auto digit = [&text](std::size_t offset) {
            return static_cast<unsigned>(text[offset] - '0');
        };
        const unsigned parsed_year = digit(0) * 1000U + digit(1) * 100U +
                                     digit(2) * 10U + digit(3);
        const unsigned parsed_month = digit(5) * 10U + digit(6);
        const unsigned parsed_day = digit(8) * 10U + digit(9);
        if (parsed_year < 1900 || parsed_month == 0 || parsed_month > 12) {
            throw TemporalValidationError(TemporalValidationErrorCode::INVALID_DATE,
                "trading date year or month is outside the supported calendar");
        }
        constexpr unsigned month_lengths[]{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        unsigned maximum_day = month_lengths[parsed_month - 1];
        const bool leap = parsed_year % 4 == 0 &&
                          (parsed_year % 100 != 0 || parsed_year % 400 == 0);
        if (parsed_month == 2 && leap) ++maximum_day;
        if (parsed_day == 0 || parsed_day > maximum_day) {
            throw TemporalValidationError(TemporalValidationErrorCode::INVALID_DATE,
                "trading date day is outside the selected month");
        }
        // Four checked year digits are bounded by 9999, so this cast is exact.
        return TradingDate{static_cast<int>(parsed_year), parsed_month, parsed_day};
    }
};

inline constexpr std::uint64_t NANOSECONDS_PER_DAY = 86400ULL * 1000000000ULL;

enum class TimestampRegressionPolicy { COUNT_ONLY, REJECT };

struct TemporalMetrics {
    std::uint64_t observations = 0;
    std::uint64_t regressions = 0;
    std::optional<std::uint64_t> last_timestamp;
};

// Checks a configured 24-hour nanoseconds-since-midnight domain. It neither
// converts a local date to Unix/UTC nor models timezone/DST/leap-second rules.
// File order is never changed. Tracking numbers and transport sequence numbers
// are not involved. A regression is relative to the immediately preceding
// accepted observation, not the maximum timestamp ever seen.
class IntradayTimestampValidator {
public:
    explicit IntradayTimestampValidator(
        TimestampRegressionPolicy policy = TimestampRegressionPolicy::COUNT_ONLY)
        : policy_(policy) {
        if (policy != TimestampRegressionPolicy::COUNT_ONLY &&
            policy != TimestampRegressionPolicy::REJECT) {
            throw std::invalid_argument("invalid timestamp regression policy");
        }
    }

    // Rejected observations, including counter overflow, leave all metrics
    // unchanged. Successful observations use only fixed-size state.
    void observe(std::uint64_t nanoseconds_since_midnight) {
        if (nanoseconds_since_midnight >= NANOSECONDS_PER_DAY) {
            throw TemporalValidationError(TemporalValidationErrorCode::OUT_OF_DAY,
                "ITCH timestamp is outside the configured 24-hour nanosecond day");
        }
        const bool regressed = metrics_.last_timestamp &&
            nanoseconds_since_midnight < *metrics_.last_timestamp;
        if (regressed && policy_ == TimestampRegressionPolicy::REJECT) {
            throw TemporalValidationError(TemporalValidationErrorCode::TIMESTAMP_REGRESSION,
                "ITCH timestamp regressed relative to the previous accepted observation");
        }
        if (metrics_.observations == std::numeric_limits<std::uint64_t>::max() ||
            (regressed && metrics_.regressions == std::numeric_limits<std::uint64_t>::max())) {
            throw TemporalValidationError(TemporalValidationErrorCode::COUNTER_OVERFLOW,
                "ITCH temporal observation counter overflow");
        }
        ++metrics_.observations;
        if (regressed) ++metrics_.regressions;
        metrics_.last_timestamp = nanoseconds_since_midnight;
    }

    const TemporalMetrics& metrics() const noexcept { return metrics_; }

private:
    TimestampRegressionPolicy policy_;
    TemporalMetrics metrics_;
};

} // namespace nasdaq_itch

#endif
