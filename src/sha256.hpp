#ifndef SHA256_HPP
#define SHA256_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace nasdaq_itch {

// Byte-oriented SHA-256 for reproducible local file-integrity checks.
// Algorithm/initial values/constants: RFC6234 sections4.1,5.1,6.1,6.2.
// https://www.rfc-editor.org/rfc/rfc6234.html
// This is not a validated cryptographic module, an HMAC/signature, proof of
// trusted provenance, data permission or protection against an attacker who
// can replace both a file and its expected digest.
class Sha256 {
public:
    using Digest = std::array<std::uint8_t, 32>;
    static constexpr std::uint64_t MAX_INPUT_BYTES =
        std::numeric_limits<std::uint64_t>::max() / 8;

    // Null/zero is a no-op before finish. All size/state/pointer checks occur
    // before mutation. The caller still owns the validity of non-null ranges.
    // Success uses fixed storage, with a 64-word schedule on transform's stack.
    void update(const std::uint8_t* data, std::size_t size) {
        if (finished_) throw std::logic_error("SHA-256 update after finish");
        if (data == nullptr && size != 0) {
            throw std::invalid_argument("SHA-256 null input with nonzero size");
        }
        if (size > MAX_INPUT_BYTES - bytes_) {
            throw std::length_error("SHA-256 input bit length exceeds uint64 range");
        }
        bytes_ += static_cast<std::uint64_t>(size);
        if (size == 0) return;
        if (used_ != 0) {
            const auto amount = std::min(buffer_.size() - used_, size);
            std::memcpy(buffer_.data() + used_, data, amount);
            used_ += amount;
            data += amount;
            size -= amount;
            if (used_ == buffer_.size()) { transform(buffer_.data()); used_ = 0; }
        }
        while (size >= buffer_.size()) {
            transform(data);
            data += buffer_.size();
            size -= buffer_.size();
        }
        if (size != 0) {
            std::memcpy(buffer_.data(), data, size);
            used_ = size;
        }
    }

    // Idempotent. Padding is internal and is not included in the checked
    // message length. No I/O, reset, dynamic storage or caller buffer mutation.
    Digest finish() noexcept {
        if (finished_) return digest_;
        const auto bit_length = bytes_ * 8;
        buffer_[used_++] = 0x80;
        if (used_ > 56) {
            std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(used_), buffer_.end(), std::uint8_t{0});
            transform(buffer_.data());
            used_ = 0;
        }
        std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(used_), buffer_.begin() + 56, std::uint8_t{0});
        for (std::size_t i = 0; i < 8; ++i) {
            buffer_[56 + i] = static_cast<std::uint8_t>(bit_length >> ((7 - i) * 8));
        }
        transform(buffer_.data());
        for (std::size_t i = 0; i < state_.size(); ++i) {
            for (std::size_t j = 0; j < 4; ++j) {
                digest_[i * 4 + j] = static_cast<std::uint8_t>(state_[i] >> ((3 - j) * 8));
            }
        }
        used_ = 0;
        finished_ = true;
        return digest_;
    }

    // Finalizes before allocating a result string. If allocation fails, the
    // digest remains finalized/idempotently available; update is not revived.
    std::string hex_digest() {
        const auto digest = finish();
        constexpr char digits[] = "0123456789abcdef";
        std::string text(64, '0');
        for (std::size_t i = 0; i < digest.size(); ++i) {
            text[i * 2] = digits[digest[i] >> 4];
            text[i * 2 + 1] = digits[digest[i] & 0x0f];
        }
        return text;
    }

private:
    static std::uint32_t rotate_right(std::uint32_t value, unsigned count) noexcept {
        // All call sites use fixed counts in 1..31, avoiding width-sized shifts.
        return (value >> count) | (value << (32 - count));
    }
    void transform(const std::uint8_t* block) noexcept {
        static constexpr std::array<std::uint32_t, 64> constants{{
            0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,
            0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
            0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,
            0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
            0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,
            0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
            0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,
            0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
            0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,
            0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
            0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,
            0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
            0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,
            0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
            0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,
            0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U
        }};
        std::array<std::uint32_t, 64> schedule{};
        for (std::size_t i = 0; i < 16; ++i) {
            const auto* word = block + i * 4;
            schedule[i] = (std::uint32_t{word[0]} << 24) | (std::uint32_t{word[1]} << 16) |
                          (std::uint32_t{word[2]} << 8) | std::uint32_t{word[3]};
        }
        for (std::size_t i = 16; i < schedule.size(); ++i) {
            const auto x = schedule[i - 15];
            const auto y = schedule[i - 2];
            const auto small0 = rotate_right(x, 7) ^ rotate_right(x, 18) ^ (x >> 3);
            const auto small1 = rotate_right(y, 17) ^ rotate_right(y, 19) ^ (y >> 10);
            schedule[i] = schedule[i - 16] + small0 + schedule[i - 7] + small1;
        }
        auto a = state_[0]; auto b = state_[1]; auto c = state_[2]; auto d = state_[3];
        auto e = state_[4]; auto f = state_[5]; auto g = state_[6]; auto h = state_[7];
        for (std::size_t i = 0; i < schedule.size(); ++i) {
            const auto large1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
            const auto choose = (e & f) ^ (~e & g);
            const std::uint32_t temporary1 = h + large1 + choose + constants[i] + schedule[i];
            const auto large0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temporary2 = large0 + majority;
            h = g; g = f; f = e; e = d + temporary1;
            d = c; c = b; b = a; a = temporary1 + temporary2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{{
        0x6a09e667U,0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,
        0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U
    }};
    std::array<std::uint8_t, 64> buffer_{};
    std::uint64_t bytes_ = 0;
    std::size_t used_ = 0;
    Digest digest_{};
    bool finished_ = false;
};

} // namespace nasdaq_itch
#endif
