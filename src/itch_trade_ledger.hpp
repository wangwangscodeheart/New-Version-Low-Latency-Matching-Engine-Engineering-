#ifndef ITCH_TRADE_LEDGER_HPP
#define ITCH_TRADE_LEDGER_HPP

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace nasdaq_itch {

enum class ReportKind { DISPLAYED_EXECUTION, PRICED_EXECUTION, NON_DISPLAYED, CROSS };
struct TradeReport {
    std::uint64_t match_number;
    std::uint16_t stock_locate;
    ReportKind kind;
    std::uint32_t price;
    std::uint64_t shares;
    bool printable;
    bool broken = false;
};
struct TradeTotals {
    std::uint64_t reports = 0;          // Accepted reports, including broken tombstones.
    std::uint64_t broken_reports = 0;
    std::uint64_t printable_reports = 0; // Net of accepted breaks.
    std::uint64_t printable_volume = 0;  // Net shares; not notional/accounting volume.
};
enum class LedgerErrorCode {
    DUPLICATE_MATCH, UNKNOWN_MATCH, WRONG_STOCK, ALREADY_BROKEN,
    UNSUPPORTED_BREAK, INVALID_REPORT, CAPACITY, COUNTER_OVERFLOW
};
class LedgerError : public std::runtime_error {
public:
    LedgerError(LedgerErrorCode code, const char* reason, std::uint64_t match_number)
        : std::runtime_error(reason), code_(code), match_number_(match_number) {}
    LedgerErrorCode code() const noexcept { return code_; }
    std::uint64_t match_number() const noexcept { return match_number_; }
private:
    LedgerErrorCode code_;
    std::uint64_t match_number_;
};

// One historical session's bounded report-correlation ledger. It neither
// changes a displayed order book nor supplies a production trade/account ledger.
// Match references are global across stocks and are retained after a break.
// Only E/C/P can be broken in this declared historical profile; Q is retained
// as a report but is deliberately outside this B-message correlation policy.
// Zero-share Q notifications are not executions and must not be passed here.
// Failed validation/allocation leaves the previous reports and totals intact.
class ItchTradeLedger {
    static constexpr std::uint32_t MAXIMUM_HISTORICAL_PRICE = 2000000000U;
    const std::size_t maximum_reports_;
    std::unordered_map<std::uint64_t, TradeReport> reports_;
    TradeTotals totals_;

    static bool valid_kind(ReportKind kind) noexcept {
        switch (kind) {
            case ReportKind::DISPLAYED_EXECUTION:
            case ReportKind::PRICED_EXECUTION:
            case ReportKind::NON_DISPLAYED:
            case ReportKind::CROSS: return true;
        }
        return false;
    }
    [[noreturn]] static void fail(LedgerErrorCode code, const char* reason,
                                  std::uint64_t match_number) {
        // No raw market payload or stock text is included in diagnostics.
        throw LedgerError(code, reason, match_number);
    }

public:
    explicit ItchTradeLedger(std::size_t maximum_reports = 1000000)
        : maximum_reports_(maximum_reports) {
        if (maximum_reports == 0) throw std::invalid_argument("trade ledger capacity must be positive");
    }
    ItchTradeLedger(const ItchTradeLedger&) = delete;
    ItchTradeLedger& operator=(const ItchTradeLedger&) = delete;
    ItchTradeLedger(ItchTradeLedger&&) = delete;
    ItchTradeLedger& operator=(ItchTradeLedger&&) = delete;

    void record(const TradeReport& report) {
        if (report.match_number == 0 || report.stock_locate == 0 || report.price == 0 ||
            report.price > MAXIMUM_HISTORICAL_PRICE || report.shares == 0 || report.broken ||
            !valid_kind(report.kind) ||
            (report.kind != ReportKind::PRICED_EXECUTION && !report.printable)) {
            fail(LedgerErrorCode::INVALID_REPORT, "invalid historical trade report", report.match_number);
        }
        if (reports_.find(report.match_number) != reports_.end()) {
            fail(LedgerErrorCode::DUPLICATE_MATCH, "trade match reference already observed", report.match_number);
        }
        if (reports_.size() >= maximum_reports_) {
            fail(LedgerErrorCode::CAPACITY, "trade report capacity exhausted", report.match_number);
        }

        // All arithmetic is checked before map insertion, and all work after
        // insertion succeeds is fixed-size/nothrow. In particular no report is
        // inserted and then rejected because printable volume cannot fit.
        auto next = totals_;
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        if (next.reports == maximum || (report.printable &&
            (next.printable_reports == maximum || report.shares > maximum - next.printable_volume))) {
            fail(LedgerErrorCode::COUNTER_OVERFLOW, "trade report totals overflow", report.match_number);
        }
        ++next.reports;
        if (report.printable) {
            ++next.printable_reports;
            next.printable_volume += report.shares;
        }
        const auto inserted = reports_.emplace(report.match_number, report);
        if (!inserted.second) {
            fail(LedgerErrorCode::DUPLICATE_MATCH, "trade match reference already observed", report.match_number);
        }
        totals_ = next;
    }

    void break_trade(std::uint16_t stock_locate, std::uint64_t match_number) {
        if (stock_locate == 0 || match_number == 0) {
            fail(LedgerErrorCode::INVALID_REPORT, "invalid broken-trade identity", match_number);
        }
        const auto found = reports_.find(match_number);
        if (found == reports_.end()) {
            fail(LedgerErrorCode::UNKNOWN_MATCH, "broken trade refers to an unknown match", match_number);
        }
        if (found->second.stock_locate != stock_locate) {
            fail(LedgerErrorCode::WRONG_STOCK, "broken trade stock differs from report", match_number);
        }
        if (found->second.broken) {
            fail(LedgerErrorCode::ALREADY_BROKEN, "trade match is already broken", match_number);
        }
        if (found->second.kind == ReportKind::CROSS) {
            fail(LedgerErrorCode::UNSUPPORTED_BREAK, "cross break is outside historical E/C/P policy", match_number);
        }

        auto next = totals_;
        if (next.broken_reports == std::numeric_limits<std::uint64_t>::max() ||
            (found->second.printable &&
             (next.printable_reports == 0 || next.printable_volume < found->second.shares))) {
            fail(LedgerErrorCode::COUNTER_OVERFLOW, "broken-trade totals cannot be represented", match_number);
        }
        ++next.broken_reports;
        if (found->second.printable) {
            --next.printable_reports;
            next.printable_volume -= found->second.shares;
        }
        found->second.broken = true;
        totals_ = next;
    }

    // Borrowed until destruction. There is no erase/reset or mutable lookup;
    // rehash does not invalidate references to unordered_map elements.
    const TradeReport* find(std::uint64_t match_number) const noexcept {
        const auto found = reports_.find(match_number);
        return found == reports_.end() ? nullptr : &found->second;
    }
    TradeTotals totals() const noexcept { return totals_; }
    std::size_t size() const noexcept { return reports_.size(); }
};

} // namespace nasdaq_itch
#endif
