#ifndef MATCHING_ENGINE_HAS_ZLIB
#define MATCHING_ENGINE_HAS_ZLIB 0
#endif
#include "itch_inspect.hpp"
#if MATCHING_ENGINE_HAS_ZLIB
#include <zlib.h>
#endif
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <initializer_list>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#define TEST_ASSERT(x) do { if (!(x)) throw std::runtime_error("assertion failed: " #x); } while (false)
namespace {
using Bytes = std::vector<std::uint8_t>;
using Args = std::vector<std::string>;
struct Invocation { int result; std::string output; std::string error; };
Invocation invoke(const Args& args) {
    std::ostringstream output, error;
    const auto result = nasdaq_itch::run_itch_inspect(args, output, error);
    return {result, output.str(), error.str()};
}
class RejectWrites : public std::streambuf {
    std::streamsize xsputn(const char*, std::streamsize) override { return 0; }
    int_type overflow(int_type) override { return traits_type::eof(); }
};
class RejectFlush : public std::stringbuf {
    int sync() override { return -1; }
};
class Fixtures {
    std::filesystem::path directory_;
    std::vector<std::filesystem::path> files_;
public:
    Fixtures() {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned i = 0; i < 100; ++i) {
            const auto candidate = std::filesystem::temp_directory_path() /
                ("dme-itch-inspect-" + std::to_string(unique) + "-" + std::to_string(i));
            if (std::filesystem::create_directory(candidate)) { directory_ = candidate; return; }
        }
        throw std::runtime_error("cannot create isolated synthetic fixture directory");
    }
    ~Fixtures() {
        // Only files registered in this freshly created directory; no recursive
        // deletion and no existing user file is ever overwritten or removed.
        std::error_code ignored;
        for (const auto& file : files_) std::filesystem::remove(file, ignored);
        std::filesystem::remove(directory_, ignored);
    }
    std::string write(const char* name, const Bytes& bytes) {
        const auto file = directory_ / name;
        if (std::filesystem::exists(file)) throw std::runtime_error("fixture already exists");
        files_.push_back(file);
        std::ofstream output(file, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (!output) throw std::runtime_error("synthetic fixture write failed");
        return file.u8string();
    }
};
Bytes control_file(std::string_view phases = "OSQMEC") {
    Bytes bytes;
    std::uint8_t tick = 0;
    for (const auto phase : phases) {
        // Invented S records only. This is not evidence of an actual market day.
        const Bytes record{0,12,'S',0,0,0,0,0,0,0,0,0,tick++,static_cast<std::uint8_t>(phase)};
        bytes.insert(bytes.end(), record.begin(), record.end());
    }
    bytes.insert(bytes.end(), {0,0});
    return bytes;
}
Args options(const std::string& path) {
    return {"--input", path, "--date", "2020-01-30", "--authorized-local-use"};
}
Args options_with(const std::string& path, std::initializer_list<std::string> additions) {
    auto args = options(path);
    args.insert(args.end(), additions.begin(), additions.end());
    return args;
}
void assert_no_success(const Invocation& invocation, int expected_result = 1) {
    TEST_ASSERT(invocation.result == expected_result);
    TEST_ASSERT(!invocation.error.empty());
    TEST_ASSERT(invocation.output.find("completion=") == std::string::npos);
    TEST_ASSERT(invocation.output.find("PROFILE_COMPLETE_NOT_REAL_DATA_ACCEPTANCE") == std::string::npos);
    TEST_ASSERT(invocation.output.find("source_sha256=") == std::string::npos);
}
constexpr const char* CONTROL_SHA256 = "3afc0c0eb85db019a30ff6231ea2442471c9d48ee0a518230cb207e504961787";
void test_help_and_preopen_guards() {
    TEST_ASSERT(invoke({"--help"}).result == 0);
    TEST_ASSERT(invoke({"--help"}).output.find("does not grant rights") != std::string::npos);
    TEST_ASSERT(invoke({}).result == 2);
    const auto denied = invoke({"--input", "not-opened.bin", "--date", "2020-01-30"});
    TEST_ASSERT(denied.result == 2 && denied.error.find("--authorized-local-use") != std::string::npos);
    for (const Args& args : {
        Args{"--input"}, Args{"--what"},
        Args{"--input","x","--date","2020-02-30","--authorized-local-use"},
        Args{"--input","x","--date","2020-01-30","--authorized-local-use","--max-reports","0"},
        Args{"--input","x","--date","2020-01-30","--authorized-local-use","--max-reports","18446744073709551616"},
        Args{"--input","x","--date","2020-01-30","--authorized-local-use","--max-reports","1x"},
        Args{"--input","x","--date","2020-01-30","--authorized-local-use","--date","2020-01-30"}}) {
        TEST_ASSERT(invoke(args).result == 2);
    }
    for (const bool flush_failure : {false, true}) {
        RejectWrites writes;
        RejectFlush flush;
        std::ostream output(flush_failure ? static_cast<std::streambuf*>(&flush) : &writes);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        std::ostringstream error;
        TEST_ASSERT(nasdaq_itch::run_itch_inspect({"--help"}, output, error) == 1);
    }
    {
        RejectWrites buffer;
        std::ostream error(&buffer);
        error.exceptions(std::ios::badbit | std::ios::failbit);
        std::ostringstream output;
        TEST_ASSERT(nasdaq_itch::run_itch_inspect({"--unknown"}, output, error) == 1);
    }
}
void test_valid_and_invalid_local_files() {
    Fixtures fixtures;
    const auto valid = fixtures.write("valid.bin", control_file());
    const auto result = invoke(options(valid));
    TEST_ASSERT(result.result == 0 && result.error.empty());
    TEST_ASSERT(result.output.find("records=6\n") != std::string::npos);
    TEST_ASSERT(result.output.find("trade_reports=0\n") != std::string::npos);
    TEST_ASSERT(result.output.find("date_authenticated=false") != std::string::npos);
    TEST_ASSERT(result.output.find("PROFILE_COMPLETE_NOT_REAL_DATA_ACCEPTANCE") != std::string::npos);
    {
        RejectWrites buffer;
        std::ostream output(&buffer);
        std::ostringstream error;
        TEST_ASSERT(nasdaq_itch::run_itch_inspect(options(valid), output, error) == 1);
        TEST_ASSERT(error.str().find("result is not accepted") != std::string::npos);
    }
    {
        RejectFlush buffer;
        std::ostream output(&buffer);
        std::ostringstream error;
        TEST_ASSERT(nasdaq_itch::run_itch_inspect(options(valid), output, error) == 1);
        TEST_ASSERT(error.str().find("flush failed") != std::string::npos);
    }
    TEST_ASSERT(invoke(options(valid + ".missing")).result == 1);
    const auto gzip = fixtures.write("gzip.bin", Bytes{0x1f,0x8b,1,2});
    TEST_ASSERT(invoke(options(gzip)).result == 1);
    const auto extension = fixtures.write("not-accepted.gz", control_file());
    TEST_ASSERT(invoke(options(extension)).result == 1);
    auto truncated = control_file();
    truncated.pop_back();
    const auto partial = fixtures.write("partial.bin", truncated);
    TEST_ASSERT(invoke(options(partial)).result == 1);
    auto out_of_day = control_file();
    for (std::size_t i = 7; i <= 12; ++i) out_of_day[i] = 0xff;
    const auto invalid_time = fixtures.write("timestamp.bin", out_of_day);
    TEST_ASSERT(invoke(options(invalid_time)).result == 1);
    auto regressed = control_file();
    regressed[14 + 12] = 5; // Next accepted tick=2 regresses from tick=5.
    const auto regression_path = fixtures.write("regressions.bin", regressed);
    const auto warning = invoke(options(regression_path));
    TEST_ASSERT(warning.result == 0 && warning.error.find("Warning:") != std::string::npos);
    TEST_ASSERT(warning.output.find("timestamp_regressions=1") != std::string::npos);
    auto reject = options(regression_path);
    reject.push_back("--reject-time-regressions");
    TEST_ASSERT(invoke(reject).result == 1);
    const auto short_phases = fixtures.write("short-phases.bin", control_file("OC"));
    TEST_ASSERT(invoke(options(short_phases)).result == 1);
    auto relaxed = options(short_phases);
    relaxed.push_back("--allow-missing-system-phases");
    const auto relaxed_result = invoke(relaxed);
    TEST_ASSERT(relaxed_result.result == 0);
    TEST_ASSERT(relaxed_result.output.find("all_system_phases_required=false") != std::string::npos);
}
void test_raw_integrity_options() {
    Fixtures fixtures;
    const auto bytes = control_file();
    TEST_ASSERT(bytes.size() == 86);
    const auto path = fixtures.write("integrity.bin", bytes);
    const auto result = invoke(options(path));
    TEST_ASSERT(result.result == 0 && result.error.empty());
    // Independent known-answer digest of the invented 86-byte fixture, not a
    // digest calculated by the implementation under test or real market data.
    TEST_ASSERT(result.output.find(std::string("source_sha256=") + CONTROL_SHA256 + '\n') != std::string::npos);
    TEST_ASSERT(result.output.find("checksum_computed=true\n") != std::string::npos);
    TEST_ASSERT(result.output.find("expected_sha256_verified=false\n") != std::string::npos);
    TEST_ASSERT(result.output.find("hash_domain=raw_source_bytes\n") != std::string::npos);
    TEST_ASSERT(result.output.find("input_compression=none\n") != std::string::npos);
    TEST_ASSERT(result.output.find("gzip_crc_and_isize_verified=not_applicable\n") != std::string::npos);
    TEST_ASSERT(result.output.find("input_bytes=86\n") != std::string::npos);
    TEST_ASSERT(result.output.find("decoded_bytes=86\n") != std::string::npos);
    TEST_ASSERT(result.output.find("gzip_members=0\n") != std::string::npos);

    const auto matching = invoke(options_with(path, {"--expected-sha256",
        "3AFC0C0EB85DB019A30FF6231EA2442471C9D48EE0A518230CB207E504961787"}));
    TEST_ASSERT(matching.result == 0 && matching.error.empty());
    TEST_ASSERT(matching.output.find("expected_sha256_verified=true\n") != std::string::npos);
    TEST_ASSERT(matching.output.find(std::string("source_sha256=") + CONTROL_SHA256 + '\n') != std::string::npos);
    assert_no_success(invoke(options_with(path, {"--expected-sha256", std::string(64, '0')})));
    for (const auto& value : {std::string(63, '0'), std::string(65, '0'), std::string(64, 'g'),
                              std::string("0x") + std::string(62, '0'), std::string(" ") + std::string(63, '0')}) {
        assert_no_success(invoke(options_with(path, {"--expected-sha256", value})), 2);
    }
    assert_no_success(invoke(options_with(path, {"--expected-sha256"})), 2);
    assert_no_success(invoke(options_with(path, {"--expected-sha256", ""})), 2);
    assert_no_success(invoke(options_with(path, {"--expected-sha256", CONTROL_SHA256,
        "--expected-sha256", CONTROL_SHA256})), 2);

    const auto exact_limits = invoke(options_with(path, {"--max-input-bytes", "86",
        "--max-decoded-bytes", "86", "--max-gzip-members", "1"}));
    TEST_ASSERT(exact_limits.result == 0);
    assert_no_success(invoke(options_with(path, {"--max-input-bytes", "85"})));
    assert_no_success(invoke(options_with(path, {"--max-decoded-bytes", "85"})));
    TEST_ASSERT(invoke(options_with(path, {"--max-input-bytes", "18446744073709551615",
        "--max-decoded-bytes", "18446744073709551615",
        "--max-gzip-members", "18446744073709551615"})).result == 0);
    for (const auto* flag : {"--max-input-bytes", "--max-decoded-bytes", "--max-gzip-members", "--max-reports"}) {
        for (const auto* value : {"0", "-1", "+1", "1x", "18446744073709551616", ""}) {
            assert_no_success(invoke(options_with(path, {flag, value})), 2);
        }
        assert_no_success(invoke(options_with(path, {flag})), 2);
        assert_no_success(invoke(options_with(path, {flag, "1", flag, "2"})), 2);
    }
    TEST_ASSERT(invoke(options_with(path, {"--compression", "none"})).result == 0);
    TEST_ASSERT(invoke(options_with(path, {"--compression", "auto"})).result == 0);
    assert_no_success(invoke(options_with(path, {"--compression", "GZIP"})), 2);
    assert_no_success(invoke(options_with(path, {"--compression"})), 2);
    assert_no_success(invoke(options_with(path, {"--compression", "auto", "--compression", "none"})), 2);

    // Explicit NONE is an intentional caller override of extension detection.
    const auto misleading_extension = fixtures.write("raw-control.GZ", bytes);
    assert_no_success(invoke(options(misleading_extension)));
    TEST_ASSERT(invoke(options_with(misleading_extension, {"--compression", "none",
        "--expected-sha256", CONTROL_SHA256})).result == 0);
}

#if MATCHING_ENGINE_HAS_ZLIB
Bytes gzip_fixture(const Bytes& bytes) {
    z_stream stream{};
    TEST_ASSERT(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8,
        Z_DEFAULT_STRATEGY) == Z_OK);
    // These fixtures contain at most 86 invented bytes; no user file or
    // unbounded input is passed to the local test compressor.
    Bytes compressed(static_cast<std::size_t>(deflateBound(&stream, static_cast<uLong>(bytes.size()))));
    stream.next_in = const_cast<Bytef*>(bytes.data());
    stream.avail_in = static_cast<uInt>(bytes.size());
    stream.next_out = compressed.data();
    stream.avail_out = static_cast<uInt>(compressed.size());
    const auto result = deflate(&stream, Z_FINISH);
    const auto size = static_cast<std::size_t>(stream.total_out);
    const auto ended = deflateEnd(&stream);
    TEST_ASSERT(result == Z_STREAM_END && ended == Z_OK);
    compressed.resize(size);
    return compressed;
}
void test_gzip_integrity_options() {
    Fixtures fixtures;
    const auto bytes = control_file();
    const auto compressed = gzip_fixture(bytes);
    const auto named = fixtures.write("synthetic.GZ", compressed);
    const auto content_detected = fixtures.write("synthetic-content.bin", compressed);
    for (const auto& path : {named, content_detected}) {
        const auto result = invoke(options(path));
        TEST_ASSERT(result.result == 0 && result.error.empty());
        TEST_ASSERT(result.output.find("input_compression=gzip\n") != std::string::npos);
        TEST_ASSERT(result.output.find("gzip_crc_and_isize_verified=true\n") != std::string::npos);
        TEST_ASSERT(result.output.find("gzip_members=1\n") != std::string::npos);
        TEST_ASSERT(result.output.find("decoded_bytes=86\n") != std::string::npos);
        TEST_ASSERT(result.output.find("input_bytes=" + std::to_string(compressed.size()) + '\n') != std::string::npos);
    }
    TEST_ASSERT(invoke(options_with(content_detected, {"--compression", "gzip"})).result == 0);
    assert_no_success(invoke(options_with(named, {"--compression", "none"})));
    // Expected digest applies to compressed source bytes, NOT decoded bytes.
    assert_no_success(invoke(options_with(named, {"--expected-sha256", CONTROL_SHA256})));
    TEST_ASSERT(invoke(options_with(named, {"--max-input-bytes", std::to_string(compressed.size()),
        "--max-decoded-bytes", "86", "--max-gzip-members", "1"})).result == 0);
    assert_no_success(invoke(options_with(named, {"--max-input-bytes", std::to_string(compressed.size() - 1)})));
    assert_no_success(invoke(options_with(named, {"--max-decoded-bytes", "85"})));

    // A member boundary need not coincide with an ITCH record or its two-byte
    // length prefix. Both readers must preserve the partial record across it.
    for (const auto split : {std::size_t{1}, std::size_t{9}, bytes.size() - 1}) {
        auto concatenated = gzip_fixture(Bytes(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(split)));
        const auto second = gzip_fixture(Bytes(bytes.begin() + static_cast<std::ptrdiff_t>(split), bytes.end()));
        concatenated.insert(concatenated.end(), second.begin(), second.end());
        const auto name = "split-" + std::to_string(split) + ".gz";
        const auto path = fixtures.write(name.c_str(), concatenated);
        const auto result = invoke(options(path));
        TEST_ASSERT(result.result == 0 && result.error.empty());
        TEST_ASSERT(result.output.find("gzip_members=2\n") != std::string::npos);
        TEST_ASSERT(result.output.find("records=6\n") != std::string::npos);
        assert_no_success(invoke(options_with(path, {"--max-gzip-members", "1"})));
    }
    auto crc = compressed;
    crc[crc.size() - 8] ^= 1;
    assert_no_success(invoke(options(fixtures.write("bad-crc.gz", crc))));
    auto isize = compressed;
    isize.back() ^= 1;
    assert_no_success(invoke(options(fixtures.write("bad-isize.gz", isize))));
    auto truncated = compressed;
    truncated.pop_back();
    assert_no_success(invoke(options(fixtures.write("truncated.gz", truncated))));
    auto trailing = compressed;
    trailing.push_back(0);
    assert_no_success(invoke(options(fixtures.write("trailing.gz", trailing))));
    // The first member already contains the native end marker. A late second
    // empty member's CRC error still must prevent any completion summary.
    auto late_crc = compressed;
    auto empty = gzip_fixture({});
    empty[empty.size() - 8] ^= 1;
    late_crc.insert(late_crc.end(), empty.begin(), empty.end());
    assert_no_success(invoke(options(fixtures.write("late-crc.gz", late_crc))));
}
#else
void test_disabled_gzip_rejection() {
    Fixtures fixtures;
    const auto path = fixtures.write("raw-control.bin", control_file());
    const auto explicit_gzip = invoke(options_with(path, {"--compression", "gzip"}));
    assert_no_success(explicit_gzip);
    TEST_ASSERT(explicit_gzip.error.find("gzip support disabled") != std::string::npos);
    const auto content = fixtures.write("gzip-content.bin", Bytes{0x1f, 0x8b, 0x08});
    const auto detected = invoke(options(content));
    assert_no_success(detected);
    TEST_ASSERT(detected.error.find("disabled") != std::string::npos);
    TEST_ASSERT(invoke({"--help"}).output.find("gzip_support=disabled") != std::string::npos);
}
#endif
} // namespace
int main() {
    try {
        test_help_and_preopen_guards();
        test_valid_and_invalid_local_files();
        test_raw_integrity_options();
#if MATCHING_ENGINE_HAS_ZLIB
        test_gzip_integrity_options();
        std::cout << "Offline ITCH inspector tests passed (gzip enabled; synthetic temporary files only).\n";
#else
        test_disabled_gzip_rejection();
        std::cout << "Offline ITCH inspector tests passed (gzip disabled; compressed success tests NOT EXECUTED; synthetic temporary files only).\n";
#endif
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Offline ITCH inspector test failed: " << exception.what() << '\n';
        return 1;
    }
}
