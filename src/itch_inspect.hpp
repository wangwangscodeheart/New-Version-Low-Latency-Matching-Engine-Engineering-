#ifndef ITCH_INSPECT_HPP
#define ITCH_INSPECT_HPP

#include "itch_verified_input.hpp"
#include <charconv>
#include <filesystem>
#include <fstream>
#include <limits>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace nasdaq_itch {
namespace detail {

// No download, data dump or output-file creation. Optional decompression is
// streaming-only and never extracts a named file. The explicit
// local-use attestation is a caller declaration, not a license verification.
inline int inspect_impl(const std::vector<std::string>& arguments,
                        std::ostream& output, std::ostream& error) {
    const auto help = [&] {
        output << "Usage: matching_engine_itch_inspect --input FILE --date YYYY-MM-DD\n"
                  "       --authorized-local-use [--max-reports N]\n"
                  "       [--reject-time-regressions] [--allow-missing-system-phases]\n"
                  "       [--compression auto|none|gzip] [--expected-sha256 HEX]\n"
                  "       [--max-input-bytes N] [--max-decoded-bytes N] [--max-gzip-members N]\n"
                  "Single-pass source SHA-256; no downloads or extracted output files.\n"
               << "gzip_support=" << (StreamingGzipDecoder::available() ? "enabled" : "disabled") << '\n'
               << "Date is caller-supplied; authorization flag does not grant rights.\n";
    };
    for (const auto& argument : arguments) {
        if (argument == "--help" || argument == "-h") {
            help();
            output.flush();
            if (!output) { error << "Help output failed.\n"; return 1; }
            return 0;
        }
    }
    std::string input_path;
    std::string date_label;
    bool authorized = false;
    bool reject_regressions = false;
    bool allow_missing_phases = false;
    bool reports_seen = false;
    bool compression_seen = false;
    bool input_limit_seen = false;
    bool output_limit_seen = false;
    bool member_limit_seen = false;
    VerifiedInputOptions input_options;
    std::size_t maximum_reports = 1000000;
    const auto usage_error = [&](const char* reason) {
        error << "Arguments rejected: " << reason << '\n';
        return 2;
    };
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto& argument = arguments[i];
        if (argument == "--authorized-local-use") {
            if (authorized) return usage_error("duplicate authorization flag");
            authorized = true;
        } else if (argument == "--reject-time-regressions") {
            if (reject_regressions) return usage_error("duplicate regression flag");
            reject_regressions = true;
        } else if (argument == "--allow-missing-system-phases") {
            if (allow_missing_phases) return usage_error("duplicate system-phase flag");
            allow_missing_phases = true;
        } else if (argument == "--input" || argument == "--date" || argument == "--max-reports" ||
                   argument == "--compression" || argument == "--expected-sha256" ||
                   argument == "--max-input-bytes" || argument == "--max-decoded-bytes" ||
                   argument == "--max-gzip-members") {
            if (i + 1 == arguments.size() || arguments[i + 1].empty() ||
                arguments[i + 1].compare(0, 2, "--") == 0) return usage_error("missing option value");
            const auto& value = arguments[++i];
            if (argument == "--input") {
                if (!input_path.empty()) return usage_error("duplicate input option");
                input_path = value;
            } else if (argument == "--date") {
                if (!date_label.empty()) return usage_error("duplicate date option");
                date_label = value;
            } else if (argument == "--compression") {
                if (compression_seen) return usage_error("duplicate compression option");
                compression_seen = true;
                if (value == "auto") input_options.compression = InputCompression::AUTO;
                else if (value == "none") input_options.compression = InputCompression::NONE;
                else if (value == "gzip") input_options.compression = InputCompression::GZIP;
                else return usage_error("compression must be auto, none or gzip");
            } else if (argument == "--expected-sha256") {
                if (input_options.expected_sha256) return usage_error("duplicate expected SHA-256");
                try { input_options.expected_sha256 = VerifiedInputReader::normalize_expected_sha256(value); }
                catch (const std::invalid_argument& exception) { return usage_error(exception.what()); }
            } else {
                bool& seen = argument == "--max-reports" ? reports_seen :
                    (argument == "--max-input-bytes" ? input_limit_seen :
                     (argument == "--max-decoded-bytes" ? output_limit_seen : member_limit_seen));
                if (seen) return usage_error("duplicate input or report limit");
                seen = true;
                std::uint64_t parsed = 0;
                const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
                if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || parsed == 0) {
                    return usage_error("invalid positive integer limit");
                }
                if (argument == "--max-reports") {
                    if (parsed > std::numeric_limits<std::size_t>::max()) return usage_error("invalid report limit");
                    maximum_reports = static_cast<std::size_t>(parsed);
                } else if (argument == "--max-input-bytes") input_options.maximum_input_bytes = parsed;
                else if (argument == "--max-decoded-bytes") input_options.maximum_output_bytes = parsed;
                else input_options.maximum_gzip_members = parsed;
            }
        } else return usage_error("unknown option");
    }
    if (input_path.empty() || date_label.empty()) return usage_error("input and date are required");
    // All checks above and this attestation precede ANY filesystem input.
    if (!authorized) return usage_error("--authorized-local-use is required before opening data");
    try { (void)TradingDate::parse(date_label); }
    catch (const TemporalValidationError& exception) { return usage_error(exception.what()); }
    try {
        const auto path = std::filesystem::u8path(input_path);
        auto extension = path.extension().u8string();
        for (char& letter : extension) {
            if (letter >= 'A' && letter <= 'Z') letter = static_cast<char>(letter - 'A' + 'a');
        }
        // AUTO recognizes a .gz declaration as requiring gzip; content magic
        // also detects gzip on any other extension. Explicit NONE overrides it.
        if (extension == ".gz" && input_options.compression == InputCompression::AUTO) {
            input_options.compression = InputCompression::GZIP;
        }
        if (input_options.compression == InputCompression::GZIP && !StreamingGzipDecoder::available()) {
            error << "Input rejected: gzip support disabled; configure ENABLE_ZLIB_INPUT=ON with zlib.\n";
            return 1;
        }
        if (!std::filesystem::is_regular_file(path)) {
            error << "Input rejected: not a readable regular file.\n";
            return 1;
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) { error << "Input rejected: cannot open file.\n"; return 1; }
        RebuildLimits limits;
        limits.maximum_trade_reports = maximum_reports;
        RebuildValidation validation;
        validation.validate_intraday_timestamps = true;
        validation.timestamp_regression_policy = reject_regressions ?
            TimestampRegressionPolicy::REJECT : TimestampRegressionPolicy::COUNT_ONLY;
        validation.require_all_system_events = !allow_missing_phases;
        validation.correlate_trade_reports = true;
        ItchBookRebuilder session(limits, validation);
        const auto integrity = VerifiedInputReader::read(input, session, input_options);
        if (!session.check_invariants()) throw std::runtime_error("reconstructed book/index invariants differ");
        const auto totals = session.trade_ledger()->totals();
        const auto regressions = session.temporal_metrics()->regressions;
        if (regressions != 0) {
            error << "Warning: timestamp regressions observed=" << regressions
                  << "; file order was preserved, not treated as transport loss.\n";
            error.flush();
            if (!error) throw std::runtime_error("inspection warning write or flush failed");
        }
        output << "profile=historical_22_correlation\n"
               << "completion=PROFILE_COMPLETE_NOT_REAL_DATA_ACCEPTANCE\n"
               << "declared_date=" << date_label << '\n'
               << "date_authenticated=false\nsource_authenticated=false\nchecksum_computed=true\n"
               << "source_sha256=" << integrity.source_sha256 << '\n'
               << "expected_sha256_verified=" << (integrity.expected_sha256_verified ? "true" : "false") << '\n'
               << "hash_domain=raw_source_bytes\n"
               << "input_compression=" << (integrity.compression == InputCompression::GZIP ? "gzip" : "none") << '\n'
               << "gzip_crc_and_isize_verified=" << (integrity.compression == InputCompression::GZIP ? "true" : "not_applicable") << '\n'
               << "input_bytes=" << integrity.input_bytes << '\n'
               << "decoded_bytes=" << integrity.decoded_bytes << '\n'
               << "gzip_members=" << integrity.gzip_members << '\n'
               << "time_basis=nanoseconds_since_midnight_no_utc_conversion\n"
               << "all_system_phases_required=" << (validation.require_all_system_events ? "true" : "false") << '\n'
               << "records=" << session.metrics().processed_records << '\n'
               << "instruments=" << session.instrument_count() << '\n'
               << "active_orders=" << session.active_order_count() << '\n'
               << "seen_order_references=" << session.seen_order_reference_count() << '\n'
               << "trade_reports=" << totals.reports << '\n'
               << "broken_reports=" << totals.broken_reports << '\n'
               << "net_printable_reports=" << totals.printable_reports << '\n'
               << "net_printable_volume=" << totals.printable_volume << '\n'
               << "timestamp_regressions=" << regressions << '\n';
        output.flush();
        if (!output) throw std::runtime_error("inspection summary write or flush failed");
        return 0;
    } catch (const std::exception& exception) {
        // Errors from decoder/rebuilder/ledger contain no raw payload dumps.
        error << "Inspection failed (result is not accepted): " << exception.what() << '\n';
        return 1;
    }
}
} // namespace detail

// Even help/argument diagnostics can throw when a borrowed stream enables
// exceptions. Keep this int-return entry consistent and never report success
// after an output failure, including a failure while reporting another error.
inline int run_itch_inspect(const std::vector<std::string>& arguments,
                            std::ostream& output, std::ostream& error) noexcept {
    try { return detail::inspect_impl(arguments, output, error); }
    catch (const std::exception& exception) {
        try { error << "Inspection output/setup failed: " << exception.what() << '\n'; }
        catch (...) {}
        return 1;
    } catch (...) { return 1; }
}

} // namespace nasdaq_itch
#endif
