#include "itch_binary_reader.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <type_traits>
#include <vector>

#define TEST_ASSERT(x) do { if (!(x)) throw std::runtime_error("assertion failed: " #x); } while (false)

namespace {
using nasdaq_itch::BinaryFileError;
using nasdaq_itch::BinaryFileErrorCode;
using nasdaq_itch::BinaryFileReader;
using nasdaq_itch::BinaryRecordView;
using nasdaq_itch::CompletionStatus;
using nasdaq_itch::EndPolicy;

// All bytes in this test are generated here, not downloaded market data.
struct CopiedRecord {
    std::vector<std::uint8_t> bytes;
    std::uint64_t ordinal;
    std::uint64_t offset;
};

void append_frame(std::vector<std::uint8_t>& wire, const std::vector<std::uint8_t>& payload) {
    TEST_ASSERT(!payload.empty() && payload.size() <= 65535);
    wire.push_back(static_cast<std::uint8_t>(payload.size() >> 8U));
    wire.push_back(static_cast<std::uint8_t>(payload.size()));
    wire.insert(wire.end(), payload.begin(), payload.end());
}

void append_terminator(std::vector<std::uint8_t>& wire) {
    wire.push_back(0);
    wire.push_back(0);
}

std::string as_string(const std::vector<std::uint8_t>& wire) {
    return {wire.begin(), wire.end()};
}

std::vector<std::uint8_t> pattern(std::size_t size) {
    std::vector<std::uint8_t> result(size);
    for (std::size_t i = 0; i < size; ++i) result[i] = static_cast<std::uint8_t>(i);
    return result;
}

auto copy_to(std::vector<CopiedRecord>& records) {
    return [&records](const BinaryRecordView& view) {
        records.push_back({{view.data, view.data + view.size}, view.ordinal, view.offset});
    };
}

template <typename Action>
void expect_error(BinaryFileErrorCode expected, Action&& action,
                  std::uint64_t ordinal, std::uint64_t offset) {
    bool caught = false;
    try {
        action();
    } catch (const BinaryFileError& error) {
        caught = true;
        TEST_ASSERT(error.code() == expected);
        TEST_ASSERT(error.ordinal() == ordinal);
        TEST_ASSERT(error.offset() == offset);
        const std::string message = error.what();
        TEST_ASSERT(message.find("record " + std::to_string(ordinal)) != std::string::npos);
        TEST_ASSERT(message.find("byte offset " + std::to_string(offset)) != std::string::npos);
    }
    TEST_ASSERT(caught);
}

void discard(const BinaryRecordView&) {}

void check_failed_reader(BinaryFileReader& reader) {
    TEST_ASSERT(reader.failed());
    const auto count = reader.records();
    const auto offset = reader.bytes_consumed();
    expect_error(BinaryFileErrorCode::FAILED_STATE,
                 [&] { reader.consume(nullptr, 0, discard); }, count + 1, offset);
    expect_error(BinaryFileErrorCode::FAILED_STATE,
                 [&] { (void)reader.finish(EndPolicy::ALLOW_BOUNDARY_EOF_ONLY); }, count + 1, offset);
    std::istringstream input(std::string(2, '\0'));
    expect_error(BinaryFileErrorCode::FAILED_STATE,
                 [&] { (void)reader.read(input, discard); }, count + 1, offset);
    TEST_ASSERT(input.tellg() == std::streampos(0));
    TEST_ASSERT(reader.records() == count && reader.bytes_consumed() == offset);
}

struct Fixture {
    std::vector<std::uint8_t> wire;
    std::vector<CopiedRecord> expected;

    Fixture() {
        for (const auto& payload : std::vector<std::vector<std::uint8_t>>{
                 {'S'}, {'A', 0, 255, 7}, pattern(257)}) {
            const auto offset = static_cast<std::uint64_t>(wire.size());
            append_frame(wire, payload);
            expected.push_back({payload, static_cast<std::uint64_t>(expected.size() + 1), offset});
        }
        append_terminator(wire);
    }
};

void compare_records(const std::vector<CopiedRecord>& actual, const std::vector<CopiedRecord>& expected) {
    TEST_ASSERT(actual.size() == expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
        TEST_ASSERT(actual[i].bytes == expected[i].bytes);
        TEST_ASSERT(actual[i].ordinal == expected[i].ordinal);
        TEST_ASSERT(actual[i].offset == expected[i].offset);
    }
}

void test_every_split_and_bytewise() {
    const Fixture fixture;
    for (std::size_t split = 0; split <= fixture.wire.size(); ++split) {
        BinaryFileReader reader;
        std::vector<CopiedRecord> actual;
        auto sink = copy_to(actual);
        reader.consume(fixture.wire.data(), split, sink);
        reader.consume(fixture.wire.data() + split, fixture.wire.size() - split, sink);
        TEST_ASSERT(reader.finish() == CompletionStatus::COMPLETE_WITH_TERMINATOR);
        TEST_ASSERT(reader.finish() == CompletionStatus::COMPLETE_WITH_TERMINATOR);
        TEST_ASSERT(reader.records() == 3 && reader.bytes_consumed() == fixture.wire.size());
        TEST_ASSERT(reader.terminated() && !reader.failed());
        compare_records(actual, fixture.expected);
    }
    BinaryFileReader bytewise;
    std::vector<CopiedRecord> actual;
    for (const auto& byte : fixture.wire) bytewise.consume(&byte, 1, copy_to(actual));
    TEST_ASSERT(bytewise.finish() == CompletionStatus::COMPLETE_WITH_TERMINATOR);
    compare_records(actual, fixture.expected);

    BinaryFileReader one_call;
    actual.clear();
    one_call.consume(fixture.wire.data(), fixture.wire.size(), copy_to(actual));
    TEST_ASSERT(one_call.finish() == CompletionStatus::COMPLETE_WITH_TERMINATOR);
    compare_records(actual, fixture.expected);
}

void test_terminator_and_boundary_policies() {
    const std::array<std::uint8_t, 2> marker{{0, 0}};
    BinaryFileReader sentinel_only;
    sentinel_only.consume(marker.data(), marker.size(), discard);
    sentinel_only.consume(nullptr, 0, discard);
    TEST_ASSERT(sentinel_only.finish() == CompletionStatus::COMPLETE_WITH_TERMINATOR);
    TEST_ASSERT(sentinel_only.records() == 0 && sentinel_only.bytes_consumed() == 2);
    TEST_ASSERT(sentinel_only.terminated() && !sentinel_only.failed());

    BinaryFileReader empty_strict;
    empty_strict.consume(nullptr, 0, discard);
    expect_error(BinaryFileErrorCode::MISSING_TERMINATOR,
                 [&] { (void)empty_strict.finish(); }, 1, 0);
    check_failed_reader(empty_strict);
    BinaryFileReader empty_lenient;
    TEST_ASSERT(empty_lenient.finish(EndPolicy::ALLOW_BOUNDARY_EOF_ONLY) ==
                CompletionStatus::BOUNDARY_EOF_WITHOUT_TERMINATOR);
    TEST_ASSERT(!empty_lenient.terminated() && !empty_lenient.failed());

    const std::array<std::uint8_t, 3> frame{{0, 1, 'A'}};
    BinaryFileReader strict;
    strict.consume(frame.data(), frame.size(), discard);
    expect_error(BinaryFileErrorCode::MISSING_TERMINATOR,
                 [&] { (void)strict.finish(); }, 2, 3);
    check_failed_reader(strict);
    BinaryFileReader lenient;
    lenient.consume(frame.data(), frame.size(), discard);
    TEST_ASSERT(lenient.finish(EndPolicy::ALLOW_BOUNDARY_EOF_ONLY) ==
                CompletionStatus::BOUNDARY_EOF_WITHOUT_TERMINATOR);
    TEST_ASSERT(lenient.records() == 1 && !lenient.terminated() && !lenient.failed());
    // A boundary-only check is not a marker and does not suppress strict checks.
    expect_error(BinaryFileErrorCode::MISSING_TERMINATOR,
                 [&] { (void)lenient.finish(); }, 2, 3);
}

void test_partial_records() {
    for (const EndPolicy policy : {EndPolicy::REQUIRE_TERMINATOR, EndPolicy::ALLOW_BOUNDARY_EOF_ONLY}) {
        const std::array<std::uint8_t, 1> prefix{{0}};
        BinaryFileReader partial_prefix;
        partial_prefix.consume(prefix.data(), prefix.size(), discard);
        expect_error(BinaryFileErrorCode::TRUNCATED_PREFIX,
                     [&] { (void)partial_prefix.finish(policy); }, 1, 0);
        TEST_ASSERT(partial_prefix.bytes_consumed() == 1);
        check_failed_reader(partial_prefix);

        const std::array<std::uint8_t, 4> payload{{0, 3, 'A', 'B'}};
        BinaryFileReader partial_payload;
        partial_payload.consume(payload.data(), payload.size(), discard);
        expect_error(BinaryFileErrorCode::TRUNCATED_PAYLOAD,
                     [&] { (void)partial_payload.finish(policy); }, 1, 0);
        TEST_ASSERT(partial_payload.records() == 0 && partial_payload.bytes_consumed() == 4);
        check_failed_reader(partial_payload);
    }
    const std::array<std::uint8_t, 6> second_partial{{0, 1, 'A', 0, 2, 'B'}};
    BinaryFileReader reader;
    reader.consume(second_partial.data(), second_partial.size(), discard);
    expect_error(BinaryFileErrorCode::TRUNCATED_PAYLOAD,
                 [&] { (void)reader.finish(); }, 2, 3);
    TEST_ASSERT(reader.records() == 1 && reader.bytes_consumed() == 6);
}

void test_trailing_bytes() {
    for (const std::vector<std::uint8_t>& bytes :
         std::vector<std::vector<std::uint8_t>>{{0, 0, 1}, {0, 0, 0, 0}}) {
        BinaryFileReader reader;
        expect_error(BinaryFileErrorCode::TRAILING_DATA,
                     [&] { reader.consume(bytes.data(), bytes.size(), discard); }, 1, 2);
        TEST_ASSERT(reader.terminated() && reader.bytes_consumed() == 2 && reader.records() == 0);
        check_failed_reader(reader);
    }
    BinaryFileReader later;
    const std::array<std::uint8_t, 2> marker{{0, 0}};
    later.consume(marker.data(), marker.size(), discard);
    TEST_ASSERT(later.finish() == CompletionStatus::COMPLETE_WITH_TERMINATOR);
    const std::uint8_t trailing = 1;
    expect_error(BinaryFileErrorCode::TRAILING_DATA,
                 [&] { later.consume(&trailing, 1, discard); }, 1, 2);
    check_failed_reader(later);
}

void test_configuration_and_oversize() {
    for (const auto limit : {std::size_t{0}, std::size_t{65536}, std::numeric_limits<std::size_t>::max()}) {
        expect_error(BinaryFileErrorCode::INVALID_CONFIGURATION,
                     [&] { BinaryFileReader invalid(limit); }, 1, 0);
    }
    BinaryFileReader smallest(1);
    const std::array<std::uint8_t, 5> one{{0, 1, 255, 0, 0}};
    smallest.consume(one.data(), one.size(), discard);
    TEST_ASSERT(smallest.finish() == CompletionStatus::COMPLETE_WITH_TERMINATOR);

    const std::array<std::uint8_t, 6> oversized{{0, 4, 'A', 'B', 'C', 'D'}};
    BinaryFileReader limited(3);
    expect_error(BinaryFileErrorCode::LENGTH_EXCEEDS_LIMIT,
                 [&] { limited.consume(oversized.data(), oversized.size(), discard); }, 1, 0);
    TEST_ASSERT(limited.bytes_consumed() == 2 && limited.records() == 0);
    check_failed_reader(limited);

    BinaryFileReader split(3);
    split.consume(oversized.data(), 1, discard);
    expect_error(BinaryFileErrorCode::LENGTH_EXCEEDS_LIMIT,
                 [&] { split.consume(oversized.data() + 1, oversized.size() - 1, discard); }, 1, 0);
    TEST_ASSERT(split.bytes_consumed() == 2);
}

void test_maximum_payload_and_fixed_storage() {
    static_assert(BinaryFileReader::MAXIMUM_PAYLOAD_SIZE == 65535);
    static_assert(BinaryFileReader::PAYLOAD_STORAGE_SIZE == 65536);
    static_assert(sizeof(BinaryFileReader) >= BinaryFileReader::PAYLOAD_STORAGE_SIZE);
    static_assert(sizeof(BinaryFileReader) <= BinaryFileReader::PAYLOAD_STORAGE_SIZE + 256);
    static_assert(!std::is_copy_constructible_v<BinaryFileReader>);
    static_assert(!std::is_copy_assignable_v<BinaryFileReader>);
    static_assert(!std::is_move_constructible_v<BinaryFileReader>);
    static_assert(!std::is_move_assignable_v<BinaryFileReader>);
    static_assert(noexcept(std::declval<const BinaryFileReader&>().records()));
    static_assert(noexcept(std::declval<const BinaryFileReader&>().bytes_consumed()));
    static_assert(noexcept(std::declval<const BinaryFileReader&>().terminated()));
    static_assert(noexcept(std::declval<const BinaryFileReader&>().failed()));
    const auto payload = pattern(65535);
    std::vector<std::uint8_t> wire;
    append_frame(wire, payload);
    append_terminator(wire);
    TEST_ASSERT(wire[0] == 255 && wire[1] == 255);
    BinaryFileReader reader;
    std::vector<CopiedRecord> actual;
    for (std::size_t offset = 0; offset < wire.size();) {
        const std::size_t count = std::min(std::size_t{97}, wire.size() - offset);
        reader.consume(wire.data() + offset, count, copy_to(actual));
        offset += count;
    }
    TEST_ASSERT(reader.finish() == CompletionStatus::COMPLETE_WITH_TERMINATOR);
    TEST_ASSERT(actual.size() == 1 && actual[0].bytes == payload);
    TEST_ASSERT(actual[0].ordinal == 1 && actual[0].offset == 0);
    TEST_ASSERT(reader.records() == 1 && reader.bytes_consumed() == 65539);
}

void test_null_and_borrowed_copy_lifetime() {
    BinaryFileReader invalid;
    expect_error(BinaryFileErrorCode::NULL_INPUT,
                 [&] { invalid.consume(nullptr, 1, discard); }, 1, 0);
    check_failed_reader(invalid);

    BinaryFileReader reader;
    std::vector<std::uint8_t> wire;
    append_frame(wire, {'A', 1, 2});
    append_frame(wire, {'Z', 8, 9});
    append_terminator(wire);
    const std::uint8_t* storage_address = nullptr;
    std::vector<CopiedRecord> copies;
    reader.consume(wire.data(), wire.size(), [&](const BinaryRecordView& view) {
        TEST_ASSERT(view.data != wire.data() + view.offset + 2);
        if (storage_address == nullptr) storage_address = view.data;
        else TEST_ASSERT(storage_address == view.data);
        copy_to(copies)(view); // Copy while the borrowed view is valid.
    });
    std::fill(wire.begin(), wire.end(), std::uint8_t{0});
    TEST_ASSERT(reader.finish() == CompletionStatus::COMPLETE_WITH_TERMINATOR);
    TEST_ASSERT(copies.size() == 2);
    TEST_ASSERT(copies[0].bytes == std::vector<std::uint8_t>({'A', 1, 2}));
    TEST_ASSERT(copies[1].bytes == std::vector<std::uint8_t>({'Z', 8, 9}));
    TEST_ASSERT(copies[1].offset == 5 && copies[1].ordinal == 2);
}

struct SinkFailure : std::runtime_error {
    SinkFailure() : std::runtime_error("synthetic sink failure") {}
};

void test_sink_exception_is_terminal() {
    const std::array<std::uint8_t, 8> wire{{0, 1, 'A', 0, 1, 'B', 0, 0}};
    BinaryFileReader reader;
    std::size_t calls = 0;
    bool caught_original = false;
    try {
        reader.consume(wire.data(), wire.size(), [&](const BinaryRecordView& view) {
            ++calls;
            TEST_ASSERT(view.ordinal == calls);
            if (calls == 2) throw SinkFailure{};
        });
    } catch (const SinkFailure&) {
        caught_original = true;
    }
    TEST_ASSERT(caught_original && calls == 2);
    TEST_ASSERT(reader.records() == 1 && reader.bytes_consumed() == 6 && !reader.terminated());
    check_failed_reader(reader);
    TEST_ASSERT(calls == 2);

    std::istringstream input(as_string({0, 1, 'A', 0, 0}));
    BinaryFileReader from_stream;
    caught_original = false;
    try {
        (void)from_stream.read(input, [](const BinaryRecordView&) { throw SinkFailure{}; });
    } catch (const SinkFailure&) {
        caught_original = true;
    }
    TEST_ASSERT(caught_original && from_stream.failed());
    TEST_ASSERT(from_stream.records() == 0 && from_stream.bytes_consumed() == 3);
}

void test_reentrant_sink_cannot_revive_reader() {
    const std::array<std::uint8_t, 3> wire{{0, 1, 'A'}};
    BinaryFileReader reader;
    expect_error(BinaryFileErrorCode::REENTRANT_USE, [&] {
        reader.consume(wire.data(), wire.size(), [&](const BinaryRecordView&) {
            (void)reader.finish(EndPolicy::ALLOW_BOUNDARY_EOF_ONLY);
        });
    }, 1, 3);
    TEST_ASSERT(reader.records() == 0);
    check_failed_reader(reader);

    BinaryFileReader swallowed;
    expect_error(BinaryFileErrorCode::FAILED_STATE, [&] {
        swallowed.consume(wire.data(), wire.size(), [&](const BinaryRecordView&) {
            try { swallowed.consume(nullptr, 0, discard); } catch (const BinaryFileError&) {}
        });
    }, 1, 3);
    TEST_ASSERT(swallowed.records() == 0 && swallowed.failed());
}

class ThrowingBuffer : public std::streambuf {
    std::streamsize xsgetn(char*, std::streamsize) override {
        throw std::runtime_error("synthetic device failure");
    }
};

enum class ReentryAction { CONSUME, FINISH, READ };

class ReentrantBuffer : public std::streambuf {
public:
    ReentrantBuffer(BinaryFileReader& reader, ReentryAction action, bool swallow)
        : reader_(reader), action_(action), swallow_(swallow) {}

    bool saw_reentry_error() const noexcept { return saw_reentry_error_; }
    BinaryFileErrorCode observed_code() const noexcept { return observed_code_; }

private:
    std::streamsize xsgetn(char* destination, std::streamsize requested) override {
        try {
            switch (action_) {
                case ReentryAction::CONSUME:
                    reader_.consume(nullptr, 0, discard);
                    break;
                case ReentryAction::FINISH:
                    (void)reader_.finish(EndPolicy::ALLOW_BOUNDARY_EOF_ONLY);
                    break;
                case ReentryAction::READ: {
                    std::istringstream nested(std::string(2, '\0'));
                    (void)reader_.read(nested, discard);
                    break;
                }
            }
        } catch (const BinaryFileError& error) {
            saw_reentry_error_ = true;
            observed_code_ = error.code();
            if (!swallow_) throw;
        }
        // Return an otherwise-valid marker even when the rejected operation
        // was swallowed. The outer reader must not accept it or complete.
        TEST_ASSERT(requested >= 2);
        destination[0] = 0;
        destination[1] = 0;
        return 2;
    }

    BinaryFileReader& reader_;
    ReentryAction action_;
    bool swallow_;
    bool saw_reentry_error_ = false;
    BinaryFileErrorCode observed_code_ = BinaryFileErrorCode::FAILED_STATE;
};

void test_streambuf_reentry_is_terminal() {
    for (const auto action : {ReentryAction::CONSUME, ReentryAction::FINISH, ReentryAction::READ}) {
        for (const bool swallow : {false, true}) {
            for (const auto mask : {std::ios::goodbit, std::ios::badbit}) {
                BinaryFileReader reader;
                ReentrantBuffer buffer(reader, action, swallow);
                std::istream input(&buffer);
                input.exceptions(mask);
                expect_error(BinaryFileErrorCode::FAILED_STATE,
                             [&] { (void)reader.read(input, discard); }, 1, 0);
                TEST_ASSERT(buffer.saw_reentry_error());
                TEST_ASSERT(buffer.observed_code() == BinaryFileErrorCode::REENTRANT_USE);
                TEST_ASSERT(reader.records() == 0 && reader.bytes_consumed() == 0);
                TEST_ASSERT(!reader.terminated());
                check_failed_reader(reader);
            }
        }
    }
}

// Generates a long stream without storing the full file, and observes the
// reader's maximum request. Each record is {length=1, payload='X'}.
class GeneratedBuffer : public std::streambuf {
public:
    explicit GeneratedBuffer(std::uint64_t frames) : payload_bytes_(frames * 3) {}
    std::streamsize largest_request() const noexcept { return largest_request_; }

private:
    std::streamsize xsgetn(char* destination, std::streamsize requested) override {
        largest_request_ = std::max(largest_request_, requested);
        const auto count = std::min(static_cast<std::uint64_t>(requested), payload_bytes_ + 2 - position_);
        for (std::uint64_t i = 0; i < count; ++i, ++position_) {
            const std::uint8_t value = position_ >= payload_bytes_ ? 0 :
                (position_ % 3 == 1 ? 1 : (position_ % 3 == 2 ? 'X' : 0));
            destination[static_cast<std::size_t>(i)] = static_cast<char>(value);
        }
        return static_cast<std::streamsize>(count);
    }

    std::uint64_t payload_bytes_;
    std::uint64_t position_ = 0;
    std::streamsize largest_request_ = 0;
};

class FailAfterMarkerBuffer : public std::streambuf {
public:
    FailAfterMarkerBuffer() {
        bytes_[0] = 0x0f;
        bytes_[1] = static_cast<char>(0xfc); // 4092-byte synthetic payload.
        bytes_[2] = 'A';
        // Remaining payload and final two-byte marker are already zero.
    }

private:
    std::streamsize xsgetn(char* destination, std::streamsize requested) override {
        if (delivered_) throw std::runtime_error("failure after marker, before physical EOF");
        TEST_ASSERT(requested == static_cast<std::streamsize>(bytes_.size()));
        std::memcpy(destination, bytes_.data(), bytes_.size());
        delivered_ = true;
        return static_cast<std::streamsize>(bytes_.size());
    }

    std::array<char, BinaryFileReader::STREAM_CHUNK_SIZE> bytes_{};
    bool delivered_ = false;
};

void test_stream_short_reads_and_exception_masks() {
    const Fixture fixture;
    for (const auto mask : {std::ios::goodbit, std::ios::failbit | std::ios::badbit | std::ios::eofbit}) {
        std::istringstream input(as_string(fixture.wire));
        input.exceptions(mask);
        BinaryFileReader reader;
        std::vector<CopiedRecord> actual;
        TEST_ASSERT(reader.read(input, copy_to(actual)) == CompletionStatus::COMPLETE_WITH_TERMINATOR);
        TEST_ASSERT(reader.finish() == CompletionStatus::COMPLETE_WITH_TERMINATOR);
        TEST_ASSERT(input.eof() && !input.bad());
        compare_records(actual, fixture.expected);
        TEST_ASSERT(reader.bytes_consumed() == fixture.wire.size() && !reader.failed());
    }
    // Exact chunk boundary needs a subsequent zero-byte read; the other sizes
    // put a prefix/marker or payload across a stream-chunk boundary.
    for (const std::size_t length : {std::size_t{4091}, std::size_t{4092}, std::size_t{4093}, std::size_t{8193}}) {
        std::vector<std::uint8_t> wire;
        append_frame(wire, pattern(length));
        append_terminator(wire);
        std::istringstream input(as_string(wire));
        input.exceptions(std::ios::failbit | std::ios::badbit | std::ios::eofbit);
        BinaryFileReader reader;
        TEST_ASSERT(reader.read(input, discard) == CompletionStatus::COMPLETE_WITH_TERMINATOR);
        TEST_ASSERT(reader.records() == 1 && reader.bytes_consumed() == wire.size());
    }
    std::istringstream empty("");
    empty.exceptions(std::ios::failbit | std::ios::badbit | std::ios::eofbit);
    BinaryFileReader lenient;
    TEST_ASSERT(lenient.read(empty, discard, EndPolicy::ALLOW_BOUNDARY_EOF_ONLY) ==
                CompletionStatus::BOUNDARY_EOF_WITHOUT_TERMINATOR);
    TEST_ASSERT(!lenient.terminated() && !lenient.failed());

    for (const auto policy : {EndPolicy::REQUIRE_TERMINATOR, EndPolicy::ALLOW_BOUNDARY_EOF_ONLY}) {
        std::istringstream truncated(as_string({0, 2, 'A'}));
        truncated.exceptions(std::ios::failbit | std::ios::badbit | std::ios::eofbit);
        BinaryFileReader reader;
        expect_error(BinaryFileErrorCode::TRUNCATED_PAYLOAD,
                     [&] { (void)reader.read(truncated, discard, policy); }, 1, 0);
        TEST_ASSERT(reader.bytes_consumed() == 3);
    }
    std::istringstream strict_empty("");
    BinaryFileReader strict;
    expect_error(BinaryFileErrorCode::MISSING_TERMINATOR,
                 [&] { (void)strict.read(strict_empty, discard); }, 1, 0);
}

void test_stream_failures() {
    for (const auto flag : {std::ios::badbit, std::ios::failbit}) {
        std::istringstream input(std::string(2, '\0'));
        input.setstate(flag);
        BinaryFileReader reader;
        expect_error(BinaryFileErrorCode::IO_ERROR,
                     [&] { (void)reader.read(input, discard); }, 1, 0);
        check_failed_reader(reader);
    }
    for (const auto mask : {std::ios::goodbit, std::ios::badbit}) {
        ThrowingBuffer buffer;
        std::istream input(&buffer);
        input.exceptions(mask);
        BinaryFileReader reader;
        expect_error(BinaryFileErrorCode::IO_ERROR,
                     [&] { (void)reader.read(input, discard); }, 1, 0);
        TEST_ASSERT(input.bad());
        check_failed_reader(reader);
    }
    FailAfterMarkerBuffer buffer;
    std::istream input(&buffer);
    input.exceptions(std::ios::badbit);
    BinaryFileReader reader;
    expect_error(BinaryFileErrorCode::IO_ERROR,
                 [&] { (void)reader.read(input, discard); }, 2, 4096);
    TEST_ASSERT(reader.terminated() && reader.records() == 1 && reader.bytes_consumed() == 4096);
    check_failed_reader(reader);
}

void test_bounded_long_stream_and_late_trailing_byte() {
    GeneratedBuffer buffer(100000);
    std::istream input(&buffer);
    BinaryFileReader reader;
    std::uint64_t observed = 0;
    TEST_ASSERT(reader.read(input, [&](const BinaryRecordView& view) {
        ++observed;
        TEST_ASSERT(view.size == 1 && view.data[0] == 'X');
        TEST_ASSERT(view.ordinal == observed && view.offset == (observed - 1) * 3);
    }) == CompletionStatus::COMPLETE_WITH_TERMINATOR);
    TEST_ASSERT(observed == 100000 && reader.records() == observed);
    TEST_ASSERT(reader.bytes_consumed() == 300002);
    TEST_ASSERT(buffer.largest_request() == static_cast<std::streamsize>(BinaryFileReader::STREAM_CHUNK_SIZE));

    std::vector<std::uint8_t> wire;
    append_frame(wire, pattern(4092));
    append_terminator(wire); // Ends exactly at the first read-chunk boundary.
    wire.push_back(0x7f);
    std::istringstream trailing(as_string(wire));
    BinaryFileReader reject;
    expect_error(BinaryFileErrorCode::TRAILING_DATA,
                 [&] { (void)reject.read(trailing, discard); }, 2, 4096);
    TEST_ASSERT(reject.terminated() && reject.records() == 1 && reject.bytes_consumed() == 4096);
    check_failed_reader(reject);
}
} // namespace

int main() {
    try {
        test_every_split_and_bytewise();
        test_terminator_and_boundary_policies();
        test_partial_records();
        test_trailing_bytes();
        test_configuration_and_oversize();
        test_maximum_payload_and_fixed_storage();
        test_null_and_borrowed_copy_lifetime();
        test_sink_exception_is_terminal();
        test_reentrant_sink_cannot_revive_reader();
        test_streambuf_reentry_is_terminal();
        test_stream_short_reads_and_exception_masks();
        test_stream_failures();
        test_bounded_long_stream_and_late_trailing_byte();
        std::cout << "Nasdaq BinaryFILE framing tests passed (synthetic bytes only).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Nasdaq BinaryFILE framing test failed: " << error.what() << '\n';
        return 1;
    }
}
