#ifndef TEMPORARY_TEST_DIRECTORY_HPP
#define TEMPORARY_TEST_DIRECTORY_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace matching_engine_test {

// Test-only ownership: reserve a new directory atomically, never borrow a
// pre-existing fixture path. Cleanup removes registered direct children and
// the now-empty directory, not arbitrary or recursively discovered contents.
class TemporaryTestDirectory {
    std::filesystem::path base_;
    std::filesystem::path path_;
    std::string prefix_;
    std::vector<std::filesystem::path> files_;
    inline static std::atomic<std::uint64_t> next_ticket_{0};

    static bool ordinary_character(char value) noexcept {
        return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
               (value >= '0' && value <= '9') || value == '_' || value == '-';
    }

public:
    explicit TemporaryTestDirectory(std::string_view label) {
        if (label.empty() || label.size() > 64) throw std::invalid_argument("Invalid test directory label");
        for (const char value : label) {
            if (!ordinary_character(value)) throw std::invalid_argument("Invalid test directory label");
        }
        prefix_ = "dme_test_" + std::string(label) + '_';
        base_ = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path());
        if (!base_.is_absolute()) throw std::runtime_error("Test temporary root is not absolute");
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto ticket = next_ticket_.fetch_add(1, std::memory_order_relaxed);
        for (unsigned attempt = 0; attempt < 1000; ++attempt) {
            auto candidate = base_ / (prefix_ + std::to_string(stamp) + '_' +
                std::to_string(ticket) + '_' + std::to_string(attempt));
            if (candidate.parent_path() != base_) throw std::runtime_error("Unsafe test directory path");
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = std::move(candidate); // noexcept ownership transfer after reservation.
                return;
            }
            if (error && error != std::errc::file_exists) {
                throw std::filesystem::filesystem_error("Cannot reserve test directory", candidate, error);
            }
        }
        throw std::runtime_error("Cannot reserve a unique test directory");
    }

    ~TemporaryTestDirectory() noexcept {
        try {
            if (path_.empty() || path_.parent_path() != base_ ||
                path_.filename().string().rfind(prefix_, 0) != 0) return;
            std::error_code ignored;
            // Do not follow a replaced directory symlink during cleanup.
            const auto status = std::filesystem::symlink_status(path_, ignored);
            if (ignored || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status)) return;
            for (const auto& file : files_) {
                if (file.parent_path() == path_) std::filesystem::remove(file, ignored);
            }
            std::filesystem::remove(path_, ignored); // Nonempty/unregistered contents are preserved.
        } catch (...) {}
    }

    TemporaryTestDirectory(const TemporaryTestDirectory&) = delete;
    TemporaryTestDirectory& operator=(const TemporaryTestDirectory&) = delete;
    TemporaryTestDirectory(TemporaryTestDirectory&&) = delete;
    TemporaryTestDirectory& operator=(TemporaryTestDirectory&&) = delete;

    const std::filesystem::path& path() const noexcept { return path_; }

    std::filesystem::path file(std::string_view name) {
        if (name.empty() || name == "." || name == "..") throw std::invalid_argument("Invalid fixture filename");
        for (const char value : name) {
            if (!ordinary_character(value) && value != '.') throw std::invalid_argument("Invalid fixture filename");
        }
        auto result = path_ / std::string(name);
        if (result.parent_path() != path_) throw std::invalid_argument("Fixture escapes its owned directory");
        files_.push_back(result); // Register before the caller can create/write it.
        return result;
    }
};

} // namespace matching_engine_test

#endif
