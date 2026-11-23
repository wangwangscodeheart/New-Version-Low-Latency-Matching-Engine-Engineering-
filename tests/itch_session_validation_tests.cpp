#include "itch_session_validation.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#define TEST_ASSERT(value) do { if (!(value)) throw std::runtime_error("assertion failed: " #value); } while (false)

namespace {
using namespace nasdaq_itch;

template<class Action>
void expect_error(TemporalValidationErrorCode code, Action&& action) {
    bool caught = false;
    try { action(); }
    catch (const TemporalValidationError& error) {
        TEST_ASSERT(error.code() == code);
        caught = true;
    }
    TEST_ASSERT(caught);
}
void expect_metrics(const TemporalMetrics& actual, const TemporalMetrics& expected) {
    TEST_ASSERT(actual.observations == expected.observations);
    TEST_ASSERT(actual.regressions == expected.regressions);
    TEST_ASSERT(actual.last_timestamp == expected.last_timestamp);
}
void check_date(std::string_view text, int year, unsigned month, unsigned day) {
    const auto parsed = TradingDate::parse(text);
    TEST_ASSERT(parsed.year == year && parsed.month == month && parsed.day == day);
}

void test_valid_calendar_dates() {
    check_date("1900-01-01", 1900, 1, 1);
    check_date("1900-02-28", 1900, 2, 28);
    check_date("2000-02-29", 2000, 2, 29);
    check_date("2020-01-30", 2020, 1, 30);
    check_date("2024-02-29", 2024, 2, 29);
    check_date("2100-02-28", 2100, 2, 28);
    check_date("2400-02-29", 2400, 2, 29);
    check_date("9999-12-31", 9999, 12, 31);
    // Calendar validation deliberately does not reject weekends or holidays.
    check_date("2026-11-22", 2026, 11, 22);
    constexpr std::array<unsigned, 12> lengths{{31,28,31,30,31,30,31,31,30,31,30,31}};
    for (unsigned month = 1; month <= 12; ++month) {
        std::string text = "2023-00-00";
        text[5] = static_cast<char>('0' + month / 10);
        text[6] = static_cast<char>('0' + month % 10);
        const unsigned day = lengths[month - 1];
        text[8] = static_cast<char>('0' + day / 10);
        text[9] = static_cast<char>('0' + day % 10);
        check_date(text, 2023, month, day);
        text[8] = static_cast<char>('0' + (day + 1) / 10);
        text[9] = static_cast<char>('0' + (day + 1) % 10);
        expect_error(TemporalValidationErrorCode::INVALID_DATE, [&] { (void)TradingDate::parse(text); });
    }
}

void test_invalid_calendar_and_shape() {
    for (const std::string_view text : {"", "20200130", "2020-1-30", "2020-01-3", "2020/01/30",
        "2020-01-30 ", " 2020-01-30", "2020-01-30\n", "2020-+1-30", "-020-01-30",
        "202A-01-30", "2020-0A-30", "2020-01-A0", "0000-01-01", "0001-01-01",
        "1899-12-31", "10000-01-01", "2020-00-01", "2020-13-01", "2020-99-01",
        "2020-01-00", "2020-01-32", "2020-04-31", "2021-02-29", "1900-02-29",
        "2100-02-29", "2200-02-29", "2300-02-29", "2020-02-30", "9999-12-32"}) {
        expect_error(TemporalValidationErrorCode::INVALID_DATE, [&] { (void)TradingDate::parse(text); });
    }
    std::string nul = "2020-01-30";
    nul[2] = '\0';
    expect_error(TemporalValidationErrorCode::INVALID_DATE, [&] { (void)TradingDate::parse(nul); });
    std::string non_ascii = "2020-01-30";
    non_ascii[5] = static_cast<char>(0xff);
    expect_error(TemporalValidationErrorCode::INVALID_DATE, [&] { (void)TradingDate::parse(non_ascii); });
    const std::array<char, 12> padded{{'x','2','0','2','0','-','0','1','-','3','0','x'}};
    check_date(std::string_view(padded.data() + 1, 10), 2020, 1, 30);
}

void test_timestamp_bounds_and_ties() {
    IntradayTimestampValidator validator;
    static_assert(noexcept(validator.metrics()));
    expect_metrics(validator.metrics(), TemporalMetrics{});
    validator.observe(0);
    validator.observe(0);
    validator.observe(NANOSECONDS_PER_DAY - 1);
    validator.observe(NANOSECONDS_PER_DAY - 1);
    expect_metrics(validator.metrics(), TemporalMetrics{4, 0, NANOSECONDS_PER_DAY - 1});
    for (const auto outside : {NANOSECONDS_PER_DAY, NANOSECONDS_PER_DAY + 1,
                              std::uint64_t{0xffffffffffffULL}, std::numeric_limits<std::uint64_t>::max()}) {
        const auto before = validator.metrics();
        expect_error(TemporalValidationErrorCode::OUT_OF_DAY, [&] { validator.observe(outside); });
        expect_metrics(validator.metrics(), before);
    }
    IntradayTimestampValidator empty;
    expect_error(TemporalValidationErrorCode::OUT_OF_DAY, [&] { empty.observe(NANOSECONDS_PER_DAY); });
    expect_metrics(empty.metrics(), TemporalMetrics{});
    empty.observe(1);
    expect_metrics(empty.metrics(), TemporalMetrics{1, 0, 1});
}

void test_regression_count_only_preserves_file_order() {
    IntradayTimestampValidator validator(TimestampRegressionPolicy::COUNT_ONLY);
    validator.observe(100);
    validator.observe(90);
    expect_metrics(validator.metrics(), TemporalMetrics{2, 1, 90});
    validator.observe(95); // Below all-time maximum but above immediate previous.
    expect_metrics(validator.metrics(), TemporalMetrics{3, 1, 95});
    validator.observe(95); // Tie is not a regression.
    validator.observe(0);
    expect_metrics(validator.metrics(), TemporalMetrics{5, 2, 0});
    validator.observe(1);
    validator.observe(0);
    expect_metrics(validator.metrics(), TemporalMetrics{7, 3, 0});
    const auto before = validator.metrics();
    expect_error(TemporalValidationErrorCode::OUT_OF_DAY, [&] { validator.observe(NANOSECONDS_PER_DAY); });
    expect_metrics(validator.metrics(), before);
}

void test_rejected_regressions_are_atomic() {
    IntradayTimestampValidator validator(TimestampRegressionPolicy::REJECT);
    validator.observe(100);
    for (const auto lower : {std::uint64_t{99}, std::uint64_t{0}, std::uint64_t{50}}) {
        const auto before = validator.metrics();
        expect_error(TemporalValidationErrorCode::TIMESTAMP_REGRESSION, [&] { validator.observe(lower); });
        expect_metrics(validator.metrics(), before);
    }
    validator.observe(100);
    validator.observe(101);
    expect_metrics(validator.metrics(), TemporalMetrics{3, 0, 101});
    const auto before = validator.metrics();
    expect_error(TemporalValidationErrorCode::OUT_OF_DAY, [&] { validator.observe(NANOSECONDS_PER_DAY); });
    expect_metrics(validator.metrics(), before);
    bool invalid_policy = false;
    try {
        IntradayTimestampValidator invalid(static_cast<TimestampRegressionPolicy>(99));
        (void)invalid;
    }
    catch (const std::invalid_argument&) { invalid_policy = true; }
    TEST_ASSERT(invalid_policy);
}
} // namespace

int main() {
    try {
        test_valid_calendar_dates();
        test_invalid_calendar_and_shape();
        test_timestamp_bounds_and_ties();
        test_regression_count_only_preserves_file_order();
        test_rejected_regressions_are_atomic();
        std::cout << "ITCH session date and intraday timestamp validation tests passed (synthetic only).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ITCH temporal validation test failed: " << error.what() << '\n';
        return 1;
    }
}
