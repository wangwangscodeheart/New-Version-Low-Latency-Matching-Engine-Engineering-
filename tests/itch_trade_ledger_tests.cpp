#include "itch_trade_ledger.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>

#define TEST_ASSERT(value) do { if (!(value)) throw std::runtime_error("assertion failed: " #value); } while (false)

namespace {
using namespace nasdaq_itch;

bool same_totals(TradeTotals left, TradeTotals right) {
    return left.reports == right.reports && left.broken_reports == right.broken_reports &&
        left.printable_reports == right.printable_reports && left.printable_volume == right.printable_volume;
}
bool same_report(const TradeReport& left, const TradeReport& right) {
    return left.match_number == right.match_number && left.stock_locate == right.stock_locate &&
        left.kind == right.kind && left.price == right.price && left.shares == right.shares &&
        left.printable == right.printable && left.broken == right.broken;
}
TradeReport report(std::uint64_t match, ReportKind kind = ReportKind::DISPLAYED_EXECUTION,
                   std::uint64_t shares = 10, std::uint16_t locate = 1,
                   std::uint32_t price = 100, bool printable = true) {
    return {match, locate, kind, price, shares, printable, false};
}
template<class Action>
void expect_error(LedgerErrorCode code, std::uint64_t match, Action&& action) {
    bool caught = false;
    try { action(); }
    catch (const LedgerError& error) {
        caught = true;
        TEST_ASSERT(error.code() == code && error.match_number() == match);
    }
    TEST_ASSERT(caught);
}
template<class Action>
void expect_unchanged(ItchTradeLedger& ledger, LedgerErrorCode code,
                      std::uint64_t match, Action&& action) {
    const auto before = ledger.totals();
    const auto count = ledger.size();
    std::array<TradeReport, 5> snapshots{};
    std::array<bool, 5> present{};
    const std::array<std::uint64_t, 5> references{{1,2,3,4,std::numeric_limits<std::uint64_t>::max()}};
    for (std::size_t i = 0; i < references.size(); ++i) {
        if (const auto* value = ledger.find(references[i])) { present[i] = true; snapshots[i] = *value; }
    }
    expect_error(code, match, action);
    TEST_ASSERT(ledger.size() == count && same_totals(ledger.totals(), before));
    for (std::size_t i = 0; i < references.size(); ++i) {
        const auto* value = ledger.find(references[i]);
        TEST_ASSERT((value != nullptr) == present[i]);
        if (value) TEST_ASSERT(same_report(*value, snapshots[i]));
    }
}

void test_printable_and_nonprintable_lifecycle() {
    ItchTradeLedger ledger;
    TEST_ASSERT(ledger.size() == 0 && same_totals(ledger.totals(), {}));
    TEST_ASSERT(ledger.find(0) == nullptr && ledger.find(1) == nullptr);
    ledger.record(report(1, ReportKind::DISPLAYED_EXECUTION, 5));
    ledger.record(report(2, ReportKind::PRICED_EXECUTION, 7, 2, 105, false));
    ledger.record(report(3, ReportKind::NON_DISPLAYED, 11));
    ledger.record(report(4, ReportKind::CROSS, 13));
    TEST_ASSERT(same_totals(ledger.totals(), {4,0,3,29}));
    TEST_ASSERT(ledger.find(2)->price == 105 && !ledger.find(2)->printable);
    ledger.break_trade(1, 1);
    TEST_ASSERT(same_totals(ledger.totals(), {4,1,2,24}));
    ledger.break_trade(2, 2); // Nonprintable C still gets a final broken tombstone.
    TEST_ASSERT(same_totals(ledger.totals(), {4,2,2,24}));
    ledger.break_trade(1, 3);
    TEST_ASSERT(same_totals(ledger.totals(), {4,3,1,13}));
    TEST_ASSERT(ledger.size() == 4 && ledger.find(1)->broken && ledger.find(2)->broken);
    TEST_ASSERT(ledger.find(3)->broken && !ledger.find(4)->broken);
    expect_unchanged(ledger, LedgerErrorCode::UNSUPPORTED_BREAK, 4, [&] { ledger.break_trade(1, 4); });
}
void test_global_match_uniqueness_and_break_failures() {
    ItchTradeLedger ledger;
    ledger.record(report(1));
    expect_unchanged(ledger, LedgerErrorCode::DUPLICATE_MATCH, 1, [&] { ledger.record(report(1)); });
    expect_unchanged(ledger, LedgerErrorCode::DUPLICATE_MATCH, 1, [&] {
        ledger.record(report(1, ReportKind::NON_DISPLAYED, 2, 2));
    });
    expect_unchanged(ledger, LedgerErrorCode::UNKNOWN_MATCH, 2, [&] { ledger.break_trade(1, 2); });
    expect_unchanged(ledger, LedgerErrorCode::WRONG_STOCK, 1, [&] { ledger.break_trade(2, 1); });
    expect_unchanged(ledger, LedgerErrorCode::INVALID_REPORT, 1, [&] { ledger.break_trade(0, 1); });
    expect_unchanged(ledger, LedgerErrorCode::INVALID_REPORT, 0, [&] { ledger.break_trade(1, 0); });
    ledger.break_trade(1, 1);
    expect_unchanged(ledger, LedgerErrorCode::ALREADY_BROKEN, 1, [&] { ledger.break_trade(1, 1); });
    expect_unchanged(ledger, LedgerErrorCode::DUPLICATE_MATCH, 1, [&] { ledger.record(report(1)); });
}
void test_invalid_reports_do_not_change_state() {
    ItchTradeLedger ledger;
    ledger.record(report(1));
    const std::array<TradeReport, 10> invalid{{
        report(0), report(2, ReportKind::DISPLAYED_EXECUTION, 0),
        report(2, ReportKind::DISPLAYED_EXECUTION, 1, 0),
        report(2, ReportKind::DISPLAYED_EXECUTION, 1, 1, 0),
        report(2, ReportKind::DISPLAYED_EXECUTION, 1, 1, 2000000001U),
        report(2, ReportKind::DISPLAYED_EXECUTION, 1, 1, std::numeric_limits<std::uint32_t>::max()),
        report(2, static_cast<ReportKind>(-1)),
        report(2, ReportKind::DISPLAYED_EXECUTION, 1, 1, 100, false),
        report(2, ReportKind::NON_DISPLAYED, 1, 1, 100, false),
        report(2, ReportKind::CROSS, 1, 1, 100, false)
    }};
    for (const auto& value : invalid) {
        expect_unchanged(ledger, LedgerErrorCode::INVALID_REPORT, value.match_number, [&] { ledger.record(value); });
    }
    auto prebroken = report(2);
    prebroken.broken = true;
    expect_unchanged(ledger, LedgerErrorCode::INVALID_REPORT, 2, [&] { ledger.record(prebroken); });
    auto invalid_high_kind = report(2);
    invalid_high_kind.kind = static_cast<ReportKind>(255);
    expect_unchanged(ledger, LedgerErrorCode::INVALID_REPORT, 2, [&] { ledger.record(invalid_high_kind); });
    ledger.record(report(2, ReportKind::PRICED_EXECUTION, 1, 1, 2000000000U, true));
    TEST_ASSERT(ledger.find(2)->price == 2000000000U && ledger.find(2)->printable);
}
void test_capacity_retains_broken_identity() {
    bool invalid = false;
    try { ItchTradeLedger impossible(0); }
    catch (const std::invalid_argument&) { invalid = true; }
    TEST_ASSERT(invalid);
    ItchTradeLedger ledger(1);
    ledger.record(report(1));
    expect_unchanged(ledger, LedgerErrorCode::CAPACITY, 2, [&] { ledger.record(report(2)); });
    ledger.break_trade(1, 1);
    expect_unchanged(ledger, LedgerErrorCode::CAPACITY, 2, [&] { ledger.record(report(2)); });
    expect_unchanged(ledger, LedgerErrorCode::DUPLICATE_MATCH, 1, [&] { ledger.record(report(1)); });
    TEST_ASSERT(same_totals(ledger.totals(), {1,1,0,0}));
}
void test_full_width_volume_overflow_and_exact_break() {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    ItchTradeLedger ledger;
    ledger.record(report(maximum, ReportKind::DISPLAYED_EXECUTION, maximum, 65535));
    TEST_ASSERT(same_totals(ledger.totals(), {1,0,1,maximum}));
    expect_unchanged(ledger, LedgerErrorCode::COUNTER_OVERFLOW, 1, [&] { ledger.record(report(1)); });
    TEST_ASSERT(ledger.find(1) == nullptr);
    ledger.record(report(2, ReportKind::PRICED_EXECUTION, maximum, 2, 100, false));
    TEST_ASSERT(same_totals(ledger.totals(), {2,0,1,maximum}));
    ledger.break_trade(65535, maximum);
    TEST_ASSERT(same_totals(ledger.totals(), {2,1,0,0}));
    ledger.record(report(1, ReportKind::NON_DISPLAYED, maximum));
    ledger.break_trade(2, 2);
    TEST_ASSERT(same_totals(ledger.totals(), {3,2,1,maximum}));
    ledger.break_trade(1, 1);
    TEST_ASSERT(same_totals(ledger.totals(), {3,3,0,0}));
    expect_unchanged(ledger, LedgerErrorCode::DUPLICATE_MATCH, maximum, [&] {
        ledger.record(report(maximum, ReportKind::DISPLAYED_EXECUTION, 1, 65535));
    });
}
void test_many_reports_and_borrowed_lookup() {
    ItchTradeLedger ledger(1000);
    ledger.record(report(1));
    const auto* first = ledger.find(1);
    for (std::uint64_t match = 2; match <= 1000; ++match) {
        ledger.record(report(match, ReportKind::NON_DISPLAYED, match, 2));
    }
    TEST_ASSERT(ledger.find(1) == first && same_report(*first, report(1)));
    TEST_ASSERT(same_totals(ledger.totals(), {1000,0,1000,500509}));
    for (std::uint64_t match = 2; match <= 1000; match += 2) ledger.break_trade(2, match);
    TEST_ASSERT(same_totals(ledger.totals(), {1000,500,500,250009}));
    TEST_ASSERT(ledger.size() == 1000 && ledger.find(1000)->broken && !ledger.find(999)->broken);
    expect_unchanged(ledger, LedgerErrorCode::CAPACITY, 1001, [&] { ledger.record(report(1001)); });
}
} // namespace

int main() {
    try {
        static_assert(!std::is_copy_constructible_v<ItchTradeLedger>);
        static_assert(!std::is_move_constructible_v<ItchTradeLedger>);
        test_printable_and_nonprintable_lifecycle();
        test_global_match_uniqueness_and_break_failures();
        test_invalid_reports_do_not_change_state();
        test_capacity_retains_broken_identity();
        test_full_width_volume_overflow_and_exact_break();
        test_many_reports_and_borrowed_lookup();
        std::cout << "ITCH trade ledger tests passed (synthetic reports only; E/C/P breaks, Q report-only).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ITCH trade ledger test failed: " << error.what() << '\n';
        return 1;
    }
}
