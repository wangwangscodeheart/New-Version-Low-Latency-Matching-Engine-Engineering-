#ifndef ITCH_BOOK_REBUILDER_HPP
#define ITCH_BOOK_REBUILDER_HPP

#include "itch_binary_reader.hpp"
#include "itch50_decoder.hpp"
#include "itch_session_validation.hpp"
#include "itch_trade_ledger.hpp"
#include "market_data_book.hpp"
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace nasdaq_itch {

struct RebuildLimits {
    std::size_t maximum_instruments = 65535;
    std::size_t maximum_active_orders = 1000000;
    std::size_t maximum_seen_order_references = 10000000;
    std::size_t maximum_market_participants_per_instrument = 1024;
    std::size_t maximum_trade_reports = 1000000;
};
// Explicit validation scope. Defaults preserve the previous visible-book
// profile; callers must opt into report/time/control correlation, not infer it.
struct RebuildValidation {
    bool validate_intraday_timestamps = false;
    TimestampRegressionPolicy timestamp_regression_policy = TimestampRegressionPolicy::COUNT_ONLY;
    bool require_all_system_events = false;
    bool correlate_trade_reports = false;
};
enum class RebuildState { AWAITING_START, REBUILDING, COMPLETE, STALE };
enum class RebuildErrorCode {
    INVALID_SESSION, INVALID_ROUTING, INVALID_REFERENCE, INVALID_MESSAGE,
    CAPACITY_EXCEEDED, FAILED_STATE, REENTRANT_USE, COUNTER_OVERFLOW
};
class RebuildError : public std::runtime_error {
public:
    RebuildError(RebuildErrorCode code, const char* reason,
                 std::uint64_t ordinal, std::uint64_t offset)
        : std::runtime_error(reason), code_(code), ordinal_(ordinal), offset_(offset) {}
    RebuildErrorCode code() const noexcept { return code_; }
    std::uint64_t ordinal() const noexcept { return ordinal_; }
    std::uint64_t offset() const noexcept { return offset_; }
private:
    RebuildErrorCode code_;
    std::uint64_t ordinal_;
    std::uint64_t offset_;
};
struct RebuildMetrics {
    std::uint64_t processed_records = 0;
    std::array<std::uint64_t, 256> records_by_type{};
    std::uint64_t errors = 0; // First terminal error only; retries do not inflate it.
};

// Latest metadata only, not an auction engine, trading permission decision,
// regulatory compliance service or a time-series/history of these messages.
struct InstrumentMetadata {
    std::optional<RegShoRestriction> reg_sho;
    std::map<FixedText<4>, MarketParticipantPosition> market_participants;
    std::optional<IpoQuotingPeriodUpdate> ipo;
    std::optional<LuldAuctionCollar> luld;
    // Q (Nasdaq), B (BX), X (PSX) have independent operational states.
    std::array<std::optional<OperationalHalt>, 3> operational_halts;
    std::optional<NetOrderImbalance> imbalance;
    std::optional<RetailPriceImprovement> retail;
};
struct MarketWideMetadata {
    std::optional<MwcbDeclineLevels> decline_levels;
    std::optional<MwcbStatus> status;
};
class VerifiedInputReader;

// Strict, single-owner historical receiver, not a matching engine or transport
// recovery service. Applies the decoder's 22 historical typed wire kinds.
// Prefix books are queryable while REBUILDING, not certified as live/complete.
// COMPLETE requires S/C AND the BinaryFILE marker AND framing finalization.
// read() drains physical EOF; consume()/finish() callers must supply all input.
// It means this declared profile was applied, not that a real feed is authentic
// or that omitted messages/market sessions can be detected from a file ordinal.
class ItchBookRebuilder {
    // A verified input operation owns the same single-writer/reentry guard
    // through physical EOF, optional gzip trailers, digest and finalization.
    friend class VerifiedInputReader;
    struct Instrument {
        StockDirectory directory;
        std::string symbol;
        MarketDataBook book;
        std::optional<StockTradingAction> trading_action;
        InstrumentMetadata metadata;
        Instrument(const StockDirectory& value, std::string name)
            : directory(value), symbol(std::move(name)) {}
    };
    struct ActiveReference {
        std::uint16_t locate;
        std::optional<FixedText<4>> attribution;
    };

    const RebuildLimits limits_;
    const RebuildValidation validation_;
    BinaryFileReader reader_;
    IntradayTimestampValidator clock_;
    ItchTradeLedger trades_;
    std::map<std::uint16_t, std::unique_ptr<Instrument>> instruments_;
    std::map<std::string, std::uint16_t, std::less<>> symbols_;
    std::unordered_map<std::uint64_t, ActiveReference> active_;
    std::unordered_set<std::uint64_t> seen_;
    RebuildState state_ = RebuildState::AWAITING_START;
    RebuildMetrics metrics_;
    MarketWideMetadata market_wide_;
    static constexpr std::uint32_t MAXIMUM_HISTORICAL_PRICE = 2000000000U;
    unsigned system_phase_ = 0;
    bool messages_closed_ = false;
    bool busy_ = false;
    std::uint64_t current_ordinal_ = 0;
    std::uint64_t current_offset_ = 0;

    static bool valid_price(std::uint32_t value) noexcept {
        return value != 0 && value <= MAXIMUM_HISTORICAL_PRICE;
    }

    void invalidate() noexcept {
        if (state_ != RebuildState::STALE) {
            state_ = RebuildState::STALE;
            ++metrics_.errors;
        }
    }
    [[noreturn]] void fail(RebuildErrorCode code, const char* reason) {
        invalidate(); // Even allocation of the diagnostic cannot revive the session.
        throw RebuildError(code, reason, current_ordinal_, current_offset_);
    }
    template<class Action>
    void operation(Action&& action, bool finishing = false) {
        if (state_ == RebuildState::STALE) {
            throw RebuildError(RebuildErrorCode::FAILED_STATE,
                "ITCH reconstruction is terminally stale", current_ordinal_, current_offset_);
        }
        if (busy_) fail(RebuildErrorCode::REENTRANT_USE, "re-entrant ITCH reconstruction");
        if (state_ == RebuildState::COMPLETE) {
            if (finishing) return;
            fail(RebuildErrorCode::INVALID_SESSION, "input after completed ITCH session");
        }
        busy_ = true;
        struct Guard { bool& busy; ~Guard() { busy = false; } } guard{busy_};
        try {
            action();
            // A streambuf must not swallow a rejected reentry and return data.
            if (state_ == RebuildState::STALE) {
                throw RebuildError(RebuildErrorCode::FAILED_STATE,
                    "ITCH reconstruction failed during callback", current_ordinal_, current_offset_);
            }
        } catch (...) {
            invalidate();
            throw;
        }
    }
    static bool same_directory(const StockDirectory& left, const StockDirectory& right) noexcept {
        const auto fields = [](const StockDirectory& value) {
            return std::tie(value.stock, value.market_category, value.financial_status,
                value.round_lot_size, value.round_lots_only, value.issue_classification,
                value.issue_sub_type, value.authenticity, value.short_sale_threshold,
                value.ipo_flag, value.luld_reference_price_tier, value.etp_flag,
                value.etp_leverage_factor, value.inverse_indicator);
        };
        return fields(left) == fields(right);
    }
    std::string checked_symbol(const FixedText<8>& value) {
        const auto name = text_view(value);
        if (name.empty()) fail(RebuildErrorCode::INVALID_ROUTING, "empty ITCH stock symbol");
        for (const char byte : name) {
            const auto character = static_cast<unsigned char>(byte);
            if (character <= 32 || character > 126) {
                fail(RebuildErrorCode::INVALID_ROUTING, "invalid ITCH stock text");
            }
        }
        return std::string(name);
    }
    Instrument& instrument(std::uint16_t code) {
        const auto found = instruments_.find(code);
        if (code == 0 || found == instruments_.end()) {
            fail(RebuildErrorCode::INVALID_ROUTING, "unknown ITCH stock locate");
        }
        return *found->second;
    }
    void check_stock(const Instrument& target, const FixedText<8>& stock) {
        // Exact fixed-width comparison also catches malformed padding/NUL bytes.
        if (target.directory.stock != stock) {
            fail(RebuildErrorCode::INVALID_ROUTING, "ITCH stock differs from directory");
        }
    }
    void ipo_update(std::uint16_t code, const IpoQuotingPeriodUpdate& value) {
        // Unlike other stock metadata, K's header locate is ALWAYS zero.
        // Its body Stock, not a manufactured instrument ID, selects the book.
        if (code != 0) fail(RebuildErrorCode::INVALID_ROUTING, "stock-specific IPO update header");
        const auto found = symbols_.find(text_view(value.stock));
        if (found == symbols_.end()) fail(RebuildErrorCode::INVALID_ROUTING, "IPO stock missing from directory");
        auto& target = instrument(found->second);
        check_stock(target, value.stock);
        if ((value.release_qualifier != 'A' && value.release_qualifier != 'C') ||
            value.quotation_release_time >= 86400 || value.ipo_price > MAXIMUM_HISTORICAL_PRICE ||
            (value.release_qualifier == 'C' && (value.quotation_release_time != 0 || value.ipo_price != 0))) {
            fail(RebuildErrorCode::INVALID_MESSAGE, "invalid historical IPO quotation update");
        }
        target.metadata.ipo = value;
    }
    void participant_update(Instrument& target, const MarketParticipantPosition& value) {
        check_stock(target, value.stock);
        const auto name = text_view(value.mpid);
        if (name.empty()) fail(RebuildErrorCode::INVALID_MESSAGE, "empty market participant identifier");
        for (const char byte : name) {
            const auto character = static_cast<unsigned char>(byte);
            if (character <= 32 || character > 126) {
                fail(RebuildErrorCode::INVALID_MESSAGE, "invalid market participant identifier");
            }
        }
        if ((value.primary_market_maker != 'Y' && value.primary_market_maker != 'N') ||
            std::string_view("NPSRL").find(value.market_maker_mode) == std::string_view::npos ||
            std::string_view("AEWSD").find(value.market_participant_state) == std::string_view::npos) {
            fail(RebuildErrorCode::INVALID_MESSAGE, "invalid market participant flags");
        }
        auto& positions = target.metadata.market_participants;
        const auto found = positions.find(value.mpid);
        if (found != positions.end()) {
            found->second = value; // Fixed-size assignment; no allocation at a full cap.
        } else {
            if (positions.size() == limits_.maximum_market_participants_per_instrument) {
                fail(RebuildErrorCode::CAPACITY_EXCEEDED, "market participant metadata limit exhausted");
            }
            positions.emplace(value.mpid, value); // Failure leaves old map unchanged.
        }
    }
    ActiveReference owner(std::uint16_t code, std::uint64_t reference) {
        const auto found = active_.find(reference);
        if (reference == 0 || found == active_.end() || found->second.locate != code) {
            fail(RebuildErrorCode::INVALID_REFERENCE, "unknown or wrong-stock ITCH order reference");
        }
        return found->second;
    }
    void check_new_reference(std::uint64_t reference) {
        if (reference == 0 || seen_.find(reference) != seen_.end()) {
            fail(RebuildErrorCode::INVALID_REFERENCE, "ITCH order reference is zero or reused");
        }
        if (seen_.size() == limits_.maximum_seen_order_references) {
            fail(RebuildErrorCode::CAPACITY_EXCEEDED, "ITCH seen-reference limit exhausted");
        }
    }
    template<class Apply>
    void stage_reference(std::uint64_t reference, ActiveReference entry, Apply&& apply) {
        // Staged reference nodes are rolled back if book insertion/replacement
        // fails. The book operation itself has a strong allocation guarantee.
        seen_.insert(reference);
        try {
            const auto inserted = active_.emplace(reference, entry);
            if (!inserted.second) fail(RebuildErrorCode::INVALID_REFERENCE, "duplicate active ITCH reference");
            try { apply(); }
            catch (...) { active_.erase(reference); throw; }
        } catch (...) { seen_.erase(reference); throw; }
    }
    void directory(std::uint16_t code, const StockDirectory& value) {
        if (code == 0) fail(RebuildErrorCode::INVALID_ROUTING, "zero stock-directory locate");
        auto name = checked_symbol(value.stock);
        const auto existing = instruments_.find(code);
        if (existing != instruments_.end()) {
            if (!same_directory(existing->second->directory, value)) {
                fail(RebuildErrorCode::INVALID_ROUTING, "stock-directory reassignment or metadata change");
            }
            return; // Exact repeated directory is harmless, not a remap.
        }
        if (symbols_.find(name) != symbols_.end()) {
            fail(RebuildErrorCode::INVALID_ROUTING, "stock symbol has multiple daily locates");
        }
        if (instruments_.size() == limits_.maximum_instruments) {
            fail(RebuildErrorCode::CAPACITY_EXCEEDED, "ITCH instrument limit exhausted");
        }
        auto value_ptr = std::make_unique<Instrument>(value, name);
        const auto reverse = symbols_.emplace(name, code);
        try { instruments_.emplace(code, std::move(value_ptr)); }
        catch (...) { symbols_.erase(reverse.first); throw; }
    }
    void system_event(std::uint16_t code, const SystemEvent& value) {
        if (code != 0) fail(RebuildErrorCode::INVALID_SESSION, "stock-specific system event");
        unsigned phase = 0;
        switch (value.event_code) {
            case 'O': phase = 1; break;
            case 'S': phase = 2; break;
            case 'Q': phase = 3; break;
            case 'M': phase = 4; break;
            case 'E': phase = 5; break;
            case 'C': phase = 6; break;
            default: fail(RebuildErrorCode::INVALID_SESSION, "unsupported ITCH system event");
        }
        if (phase <= system_phase_ || (system_phase_ == 0 && phase != 1)) {
            fail(RebuildErrorCode::INVALID_SESSION, "invalid ITCH system-event progression");
        }
        if (validation_.require_all_system_events && phase != system_phase_ + 1) {
            fail(RebuildErrorCode::INVALID_SESSION, "missing required ITCH system event");
        }
        // This scope allows missing intermediate phases, not arbitrary resets.
        // S/E still permits deletes/breaks; only S/C closes business messages.
        system_phase_ = phase;
        messages_closed_ = phase == 6;
        state_ = RebuildState::REBUILDING;
    }
    void add(std::uint16_t code, Instrument& target, const AddOrder& value) {
        check_stock(target, value.stock);
        check_new_reference(value.order_reference);
        if ((value.side != 'B' && value.side != 'S') || value.shares == 0 || !valid_price(value.price)) {
            fail(RebuildErrorCode::INVALID_MESSAGE, "invalid native ITCH add");
        }
        if (active_.size() == limits_.maximum_active_orders) {
            fail(RebuildErrorCode::CAPACITY_EXCEEDED, "ITCH active-order limit exhausted");
        }
        stage_reference(value.order_reference, ActiveReference{code, value.attribution}, [&] {
            if (!target.book.add_order(value.order_reference, Price(value.price),
                                      Quantity(value.shares), value.side == 'B' ? Side::BUY : Side::SELL)) {
                fail(RebuildErrorCode::INVALID_MESSAGE, "native ITCH add cannot be applied");
            }
        });
    }
    void reduce(std::uint16_t code, Instrument& target, std::uint64_t reference, std::uint32_t shares) {
        (void)owner(code, reference);
        const auto* old = target.book.find_order(reference);
        if (old == nullptr || shares == 0 || shares > old->remaining.get()) {
            fail(RebuildErrorCode::INVALID_MESSAGE, "invalid native ITCH quantity reduction");
        }
        const bool remove = shares == old->remaining.get();
        if (!target.book.reduce_order(reference, Quantity(shares))) {
            fail(RebuildErrorCode::INVALID_MESSAGE, "native ITCH reduction cannot be applied");
        }
        if (remove) active_.erase(reference);
    }
    void replace(std::uint16_t code, Instrument& target, const OrderReplace& value) {
        const auto inherited = owner(code, value.original_order_reference);
        check_new_reference(value.new_order_reference);
        if (value.shares == 0 || !valid_price(value.price)) {
            fail(RebuildErrorCode::INVALID_MESSAGE, "invalid native ITCH replacement");
        }
        stage_reference(value.new_order_reference, inherited, [&] {
            if (!target.book.replace_order(value.original_order_reference, value.new_order_reference,
                                          Price(value.price), Quantity(value.shares))) {
                fail(RebuildErrorCode::INVALID_MESSAGE, "native ITCH replacement cannot be applied");
            }
        });
        active_.erase(value.original_order_reference); // No stale iterator across emplace/rehash.
    }
    std::uint32_t execution_resting_price(std::uint16_t code, const Instrument& target,
                                         std::uint64_t reference, std::uint32_t shares) {
        (void)owner(code, reference);
        const auto* old = target.book.find_order(reference);
        if (old == nullptr || shares == 0 || shares > old->remaining.get()) {
            fail(RebuildErrorCode::INVALID_MESSAGE, "invalid correlated ITCH execution");
        }
        return static_cast<std::uint32_t>(old->price.get());
    }
    void report(ReportKind kind, std::uint16_t code, std::uint64_t match,
                std::uint32_t price, std::uint64_t shares, bool printable) {
        if (!validation_.correlate_trade_reports) return;
        trades_.record(TradeReport{match, code, kind, price, shares, printable, false});
        // A user allocation handler must not swallow rejected reentry and
        // cause a later displayed-quantity mutation in a stale session.
        if (state_ == RebuildState::STALE) {
            fail(RebuildErrorCode::FAILED_STATE, "ITCH session failed during report allocation");
        }
    }
    void on_record(const BinaryRecordView& record) {
        if (state_ == RebuildState::STALE) {
            throw RebuildError(RebuildErrorCode::FAILED_STATE, "ITCH session is stale",
                               record.ordinal, record.offset);
        }
        current_ordinal_ = record.ordinal;
        current_offset_ = record.offset;
        if (messages_closed_) fail(RebuildErrorCode::INVALID_SESSION, "payload after ITCH end-of-messages");
        if (metrics_.processed_records == std::numeric_limits<std::uint64_t>::max() ||
            record.ordinal != metrics_.processed_records + 1) {
            fail(RebuildErrorCode::COUNTER_OVERFLOW, "invalid local ITCH record ordinal");
        }
        const auto message = Itch50Decoder::decode(record.data, record.size);
        auto next_clock = clock_;
        if (validation_.validate_intraday_timestamps) {
            next_clock.observe(message.header.nanoseconds_since_midnight);
        }
        const auto kind = static_cast<unsigned char>(message.header.type);
        if (metrics_.records_by_type[kind] == std::numeric_limits<std::uint64_t>::max()) {
            fail(RebuildErrorCode::COUNTER_OVERFLOW, "ITCH type counter overflow");
        }
        if (state_ == RebuildState::AWAITING_START &&
            (message.header.type != 'S' || std::get<SystemEvent>(message.payload).event_code != 'O')) {
            fail(RebuildErrorCode::INVALID_SESSION, "ITCH reconstruction requires S/O first");
        }
        const auto code = message.header.stock_locate;
        if (message.header.type == 'S') system_event(code, std::get<SystemEvent>(message.payload));
        else if (message.header.type == 'R') directory(code, std::get<StockDirectory>(message.payload));
        else if (message.header.type == 'V' || message.header.type == 'W') {
            if (code != 0) fail(RebuildErrorCode::INVALID_ROUTING, "stock-specific market-wide message");
            if (message.header.type == 'V') {
                // Price(8), not Price(4): preserve raw full-width levels and
                // zeros without applying a stock-price cap or invented order.
                market_wide_.decline_levels = std::get<MwcbDeclineLevels>(message.payload);
            } else {
                const auto value = std::get<MwcbStatus>(message.payload);
                if (value.breached_level != '1' && value.breached_level != '2' && value.breached_level != '3') {
                    fail(RebuildErrorCode::INVALID_MESSAGE, "invalid MWCB breached level");
                }
                market_wide_.status = value;
            }
        }
        else if (message.header.type == 'K') ipo_update(code, std::get<IpoQuotingPeriodUpdate>(message.payload));
        else {
            auto& target = instrument(code);
            switch (message.header.type) {
                case 'H': {
                    const auto& value = std::get<StockTradingAction>(message.payload);
                    check_stock(target, value.stock);
                    if (value.trading_state != 'H' && value.trading_state != 'P' &&
                        value.trading_state != 'Q' && value.trading_state != 'T') {
                        fail(RebuildErrorCode::INVALID_MESSAGE, "invalid stock trading state");
                    }
                    target.trading_action = value; // Halt/quote-only does not erase displayed orders.
                    break;
                }
                case 'Y': {
                    const auto& value = std::get<RegShoRestriction>(message.payload);
                    check_stock(target, value.stock);
                    if (value.action != '0' && value.action != '1' && value.action != '2') {
                        fail(RebuildErrorCode::INVALID_MESSAGE, "invalid Reg SHO action");
                    }
                    target.metadata.reg_sho = value;
                    break;
                }
                case 'L': participant_update(target, std::get<MarketParticipantPosition>(message.payload)); break;
                case 'J': {
                    const auto& value = std::get<LuldAuctionCollar>(message.payload);
                    check_stock(target, value.stock);
                    if (value.reference_price > MAXIMUM_HISTORICAL_PRICE ||
                        value.upper_price > MAXIMUM_HISTORICAL_PRICE || value.lower_price > MAXIMUM_HISTORICAL_PRICE) {
                        fail(RebuildErrorCode::INVALID_MESSAGE, "invalid LULD collar price width");
                    }
                    // Preserve raw zero/unordered fields, rather than invent a
                    // reopening auction rule not validated against a session.
                    target.metadata.luld = value;
                    break;
                }
                case 'h': {
                    const auto& value = std::get<OperationalHalt>(message.payload);
                    check_stock(target, value.stock);
                    std::size_t venue = 0;
                    switch (value.market_code) {
                        case 'Q': venue = 0; break;
                        case 'B': venue = 1; break;
                        case 'X': venue = 2; break;
                        default: fail(RebuildErrorCode::INVALID_MESSAGE, "invalid operational halt venue");
                    }
                    if (value.action != 'H' && value.action != 'T') {
                        fail(RebuildErrorCode::INVALID_MESSAGE, "invalid operational halt action");
                    }
                    target.metadata.operational_halts[venue] = value;
                    break;
                }
                case 'I': {
                    const auto& value = std::get<NetOrderImbalance>(message.payload);
                    check_stock(target, value.stock);
                    if (std::string_view("BSNO").find(value.imbalance_direction) == std::string_view::npos ||
                        std::string_view("OCH").find(value.cross_type) == std::string_view::npos ||
                        std::string_view("L123456789ABC ").find(value.price_variation_indicator) == std::string_view::npos ||
                        value.far_price > MAXIMUM_HISTORICAL_PRICE || value.near_price > MAXIMUM_HISTORICAL_PRICE ||
                        value.current_reference_price > MAXIMUM_HISTORICAL_PRICE) {
                        fail(RebuildErrorCode::INVALID_MESSAGE, "invalid historical NOII fields");
                    }
                    target.metadata.imbalance = value; // Zero shares/prices can mean uncalculable.
                    break;
                }
                case 'N': {
                    const auto& value = std::get<RetailPriceImprovement>(message.payload);
                    check_stock(target, value.stock);
                    if (std::string_view("BSAN").find(value.interest) == std::string_view::npos) {
                        fail(RebuildErrorCode::INVALID_MESSAGE, "invalid retail interest indicator");
                    }
                    target.metadata.retail = value; // Legacy profile; not proof of 2020 emissions.
                    break;
                }
                case 'A': case 'F': add(code, target, std::get<AddOrder>(message.payload)); break;
                case 'E': {
                    const auto& value = std::get<OrderExecuted>(message.payload);
                    if (value.match_number == 0) fail(RebuildErrorCode::INVALID_MESSAGE, "zero execution match");
                    if (validation_.correlate_trade_reports) {
                        const auto price = execution_resting_price(code, target, value.order_reference, value.executed_shares);
                        report(ReportKind::DISPLAYED_EXECUTION, code, value.match_number, price, value.executed_shares, true);
                    }
                    reduce(code, target, value.order_reference, value.executed_shares);
                    break;
                }
                case 'C': {
                    const auto& value = std::get<OrderExecutedWithPrice>(message.payload);
                    if (value.match_number == 0 || !valid_price(value.execution_price)) {
                        fail(RebuildErrorCode::INVALID_MESSAGE, "invalid priced execution");
                    }
                    if (validation_.correlate_trade_reports) {
                        (void)execution_resting_price(code, target, value.order_reference, value.executed_shares);
                        report(ReportKind::PRICED_EXECUTION, code, value.match_number,
                               value.execution_price, value.executed_shares, value.printable == 'Y');
                    }
                    reduce(code, target, value.order_reference, value.executed_shares);
                    break;
                }
                case 'X': {
                    const auto& value = std::get<OrderCancel>(message.payload);
                    reduce(code, target, value.order_reference, value.cancelled_shares);
                    break;
                }
                case 'D': {
                    const auto reference = std::get<OrderDelete>(message.payload).order_reference;
                    (void)owner(code, reference);
                    if (!target.book.erase_order(reference)) {
                        fail(RebuildErrorCode::INVALID_REFERENCE, "native ITCH delete cannot be applied");
                    }
                    active_.erase(reference);
                    break;
                }
                case 'U': replace(code, target, std::get<OrderReplace>(message.payload)); break;
                case 'P': {
                    const auto& value = std::get<NonDisplayedTrade>(message.payload);
                    check_stock(target, value.stock);
                    if (value.order_reference != 0 || value.side != 'B' || value.shares == 0 ||
                        !valid_price(value.price) || value.match_number == 0) {
                        fail(RebuildErrorCode::INVALID_MESSAGE, "invalid historical non-displayed trade");
                    }
                    report(ReportKind::NON_DISPLAYED, code, value.match_number, value.price, value.shares, true);
                    break; // Never a displayed mutation/aggressor inference.
                }
                case 'Q': {
                    const auto& value = std::get<CrossTrade>(message.payload);
                    check_stock(target, value.stock);
                    // Zero-share cross notifications are valid. Their raw
                    // price/match are preserved structurally, not invented.
                    if (value.cross_price > MAXIMUM_HISTORICAL_PRICE ||
                        (value.shares != 0 && (!valid_price(value.cross_price) || value.match_number == 0)) ||
                        (value.cross_type != 'O' && value.cross_type != 'C' &&
                         value.cross_type != 'H' && value.cross_type != 'I')) {
                        fail(RebuildErrorCode::INVALID_MESSAGE, "invalid historical cross trade");
                    }
                    if (value.shares != 0) {
                        report(ReportKind::CROSS, code, value.match_number, value.cross_price, value.shares, true);
                    }
                    break;
                }
                case 'B': {
                    if (std::get<BrokenTrade>(message.payload).match_number == 0) {
                        fail(RebuildErrorCode::INVALID_MESSAGE, "zero broken-trade match");
                    }
                    if (validation_.correlate_trade_reports) {
                        trades_.break_trade(code, std::get<BrokenTrade>(message.payload).match_number);
                    }
                    break; // A report break never restores displayed shares.
                }
                default: fail(RebuildErrorCode::INVALID_MESSAGE, "unhandled typed ITCH message");
            }
        }
        if (validation_.validate_intraday_timestamps) clock_ = next_clock;
        ++metrics_.records_by_type[kind];
        ++metrics_.processed_records;
    }
    void finalize() {
        // A custom streambuf may swallow a rejected native API reentry and
        // return just a marker/EOF, with no payload callback to catch it.
        if (state_ == RebuildState::STALE) {
            fail(RebuildErrorCode::FAILED_STATE, "ITCH reconstruction failed before finalization");
        }
        if (!messages_closed_) fail(RebuildErrorCode::INVALID_SESSION, "missing ITCH S/C message");
        state_ = RebuildState::COMPLETE;
    }

public:
    explicit ItchBookRebuilder(RebuildLimits limits = {}, RebuildValidation validation = {})
        : limits_(limits), validation_(validation), clock_(validation.timestamp_regression_policy),
          trades_(limits.maximum_trade_reports) {
        if (limits.maximum_instruments == 0 || limits.maximum_instruments > 65535 ||
            limits.maximum_active_orders == 0 || limits.maximum_seen_order_references == 0 ||
            limits.maximum_market_participants_per_instrument == 0 || limits.maximum_trade_reports == 0) {
            throw std::invalid_argument("invalid ITCH reconstruction limits");
        }
    }
    ItchBookRebuilder(const ItchBookRebuilder&) = delete;
    ItchBookRebuilder& operator=(const ItchBookRebuilder&) = delete;
    ItchBookRebuilder(ItchBookRebuilder&&) = delete;
    ItchBookRebuilder& operator=(ItchBookRebuilder&&) = delete;

    void consume(const std::uint8_t* data, std::size_t size) {
        operation([&] { reader_.consume(data, size, [this](const BinaryRecordView& value) { on_record(value); }); });
    }
    void read(std::istream& input) {
        operation([&] {
            (void)reader_.read(input, [this](const BinaryRecordView& value) { on_record(value); });
            finalize();
        });
    }
    void finish() {
        operation([&] { (void)reader_.finish(); finalize(); }, true);
    }
    RebuildState state() const noexcept { return state_; }
    const RebuildMetrics& metrics() const noexcept { return metrics_; }
    std::size_t instrument_count() const noexcept { return instruments_.size(); }
    std::size_t active_order_count() const noexcept { return active_.size(); }
    std::size_t seen_order_reference_count() const noexcept { return seen_.size(); }
    const ItchTradeLedger* trade_ledger() const noexcept {
        return validation_.correlate_trade_reports && state_ != RebuildState::STALE ? &trades_ : nullptr;
    }
    const TemporalMetrics* temporal_metrics() const noexcept {
        return validation_.validate_intraday_timestamps && state_ != RebuildState::STALE ? &clock_.metrics() : nullptr;
    }
    const RebuildValidation& validation_config() const noexcept { return validation_; }
    const MarketDataBook* book(std::uint16_t code) const noexcept {
        if (state_ == RebuildState::STALE) return nullptr;
        const auto found = instruments_.find(code);
        return found == instruments_.end() ? nullptr : &found->second->book;
    }
    std::optional<std::uint16_t> locate(std::string_view name) const {
        const auto found = symbols_.find(name);
        if (found == symbols_.end()) return std::nullopt;
        return found->second;
    }
    std::string_view symbol(std::uint16_t code) const {
        const auto found = instruments_.find(code);
        return found == instruments_.end() ? std::string_view{} : std::string_view(found->second->symbol);
    }
    const StockTradingAction* trading_action(std::uint16_t code) const noexcept {
        if (state_ == RebuildState::STALE) return nullptr;
        const auto found = instruments_.find(code);
        if (found == instruments_.end() || !found->second->trading_action) return nullptr;
        return &*found->second->trading_action;
    }
    const InstrumentMetadata* metadata(std::uint16_t code) const noexcept {
        if (state_ == RebuildState::STALE) return nullptr;
        const auto found = instruments_.find(code);
        return found == instruments_.end() ? nullptr : &found->second->metadata;
    }
    const MarketWideMetadata* market_wide_metadata() const noexcept {
        return state_ == RebuildState::STALE ? nullptr : &market_wide_;
    }
    std::optional<FixedText<4>> attribution(std::uint64_t reference) const noexcept {
        if (state_ == RebuildState::STALE) return std::nullopt;
        const auto found = active_.find(reference);
        return found == active_.end() ? std::nullopt : found->second.attribution;
    }
    // Cold diagnostic, not a crossed-market test (auctions/halts can cross).
    bool check_invariants() const {
        std::size_t orders = 0;
        if (instruments_.size() != symbols_.size() || active_.size() > seen_.size()) return false;
        for (const auto& entry : instruments_) {
            const auto& target = *entry.second;
            if (!target.book.check_invariants()) return false;
            const auto reverse = symbols_.find(target.symbol);
            if (reverse == symbols_.end() || reverse->second != entry.first) return false;
            for (const auto& order : target.book.capture_state().orders) {
                const auto reference = active_.find(order.order_id);
                if (reference == active_.end() || reference->second.locate != entry.first ||
                    seen_.find(order.order_id) == seen_.end()) return false;
                ++orders;
            }
        }
        return orders == active_.size();
    }
};

} // namespace nasdaq_itch
#endif
