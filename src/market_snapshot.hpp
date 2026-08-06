#ifndef MARKET_SNAPSHOT_HPP
#define MARKET_SNAPSHOT_HPP

#include "market_manager.hpp"
#include "snapshot_file.hpp"
#include "snapshot_recovery.hpp"
#include <filesystem>
#include <fstream>
#include <memory>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef ERROR
#undef ERROR
#endif
#endif

struct MarketSnapshotRecoveryResult {
    uint64_t snapshot_sequence = 0;
    std::unique_ptr<MarketManager> markets;
    std::string error;
    bool success() const noexcept { return markets != nullptr && error.empty(); }
};

class MarketSnapshotStore {
    static uint64_t checksum(const std::string& text) noexcept {
        uint64_t hash = 1469598103934665603ULL;
        for (unsigned char byte : text) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    static uint64_t parse_u64(const std::string& text) {
        size_t consumed = 0;
        const uint64_t value = std::stoull(text, &consumed);
        if (consumed != text.size()) throw std::runtime_error("Invalid manifest integer");
        return value;
    }

    static void replace_manifest(const std::filesystem::path& temporary,
                                 const std::filesystem::path& current) {
#ifdef _WIN32
        HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot flush manifest");
        const BOOL flushed = FlushFileBuffers(file);
        CloseHandle(file);
        if (!flushed || !MoveFileExW(temporary.c_str(), current.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("Atomic manifest replacement failed");
        }
#else
        std::filesystem::rename(temporary, current);
#endif
    }

public:
    static void save_atomic(const std::filesystem::path& manifest,
                            uint64_t snapshot_sequence,
                            const MarketManager& markets) {
        const std::filesystem::path directory = manifest.parent_path().empty()
            ? std::filesystem::current_path() : manifest.parent_path();
        if (manifest.filename().string().find_first_of(",\r\n") != std::string::npos) {
            throw std::runtime_error("Unsafe snapshot manifest filename");
        }
        std::ostringstream body;
        body << "DME_MARKET_SNAPSHOT,1," << snapshot_sequence << ','
             << markets.symbol_count() << '\n';
        size_t index = 0;
        for (const Symbol& symbol : markets.symbols()) {
            const OrderBook* book = markets.find_book(symbol);
            if (!book) throw std::runtime_error("Symbol disappeared during snapshot");
            if (symbol.value().find_first_of(",\r\n") != std::string::npos) {
                throw std::runtime_error("Symbol cannot be encoded in manifest");
            }
            const std::string filename = manifest.filename().string() + ".g" +
                std::to_string(snapshot_sequence) + "." + std::to_string(index++) + ".book";
            SnapshotFile::save_atomic(directory / filename, snapshot_sequence,
                                      book->create_snapshot());
            body << symbol.value() << ',' << filename << ',' << book->capacity() << '\n';
        }
        const std::string payload = body.str();
        std::filesystem::path temporary = manifest;
        temporary += ".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) throw std::runtime_error("Cannot create snapshot manifest");
            output << payload << "CHECKSUM," << checksum(payload) << '\n';
            output.flush();
            if (!output) throw std::runtime_error("Cannot write snapshot manifest");
        }
        // Validate every generation file and the temporary manifest before the
        // single commit point makes this generation visible.
        const MarketSnapshotRecoveryResult verified = load(temporary);
        if (!verified.success()) throw std::runtime_error(verified.error);
        replace_manifest(temporary, manifest);
    }

    static MarketSnapshotRecoveryResult load(const std::filesystem::path& manifest) noexcept {
        try {
            std::ifstream input(manifest, std::ios::binary);
            if (!input) throw std::runtime_error("Cannot open snapshot manifest");
            std::vector<std::string> lines;
            std::string line;
            while (std::getline(input, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                lines.push_back(line);
            }
            if (lines.size() < 2 || lines.back().rfind("CHECKSUM,", 0) != 0) {
                throw std::runtime_error("Truncated snapshot manifest");
            }
            std::string payload;
            for (size_t i = 0; i + 1 < lines.size(); ++i) payload += lines[i] + '\n';
            if (parse_u64(lines.back().substr(9)) != checksum(payload)) {
                throw std::runtime_error("Snapshot manifest checksum mismatch");
            }
            std::vector<std::string> header;
            std::stringstream header_stream(lines.front());
            while (std::getline(header_stream, line, ',')) header.push_back(line);
            if (header.size() != 4 || header[0] != "DME_MARKET_SNAPSHOT" || header[1] != "1") {
                throw std::runtime_error("Unsupported snapshot manifest");
            }
            const uint64_t sequence = parse_u64(header[2]);
            const uint64_t count = parse_u64(header[3]);
            if (count == 0 || count != lines.size() - 2 || count > 100'000) {
                throw std::runtime_error("Invalid snapshot manifest count");
            }
            auto restored_markets = std::make_unique<MarketManager>(EmptyMarketManagerTag{});
            const std::filesystem::path directory = manifest.parent_path().empty()
                ? std::filesystem::current_path() : manifest.parent_path();
            for (size_t i = 1; i + 1 < lines.size(); ++i) {
                std::vector<std::string> fields;
                std::stringstream row(lines[i]);
                while (std::getline(row, line, ',')) fields.push_back(line);
                if (fields.size() != 3 || fields[0].empty()) {
                    throw std::runtime_error("Invalid snapshot manifest row");
                }
                const std::filesystem::path filename(fields[1]);
                if (filename != filename.filename()) {
                    throw std::runtime_error("Unsafe snapshot generation path");
                }
                const uint64_t capacity_value = parse_u64(fields[2]);
                if (capacity_value == 0 ||
                    capacity_value > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
                    throw std::runtime_error("Invalid snapshot book capacity");
                }
                const size_t capacity = static_cast<size_t>(capacity_value);
                const SnapshotFileData file = SnapshotFile::load(directory / filename);
                if (file.snapshot_sequence != sequence) {
                    throw std::runtime_error("Mixed snapshot generations");
                }
                SnapshotRestoreResult restored = SnapshotRecovery::restore(file.snapshot, capacity);
                if (!restored.success()) {
                    throw std::runtime_error(std::string("Invalid book snapshot: ") +
                                             to_string(restored.error));
                }
                const Symbol symbol(fields[0]);
                if (restored_markets->contains(symbol)) {
                    throw std::runtime_error("Duplicate symbol in snapshot manifest");
                }
                restored_markets->install_restored_book(symbol,
                                                        std::move(restored.book));
            }
            return MarketSnapshotRecoveryResult{sequence, std::move(restored_markets), {}};
        } catch (const std::exception& error) {
            return MarketSnapshotRecoveryResult{0, nullptr, error.what()};
        }
    }
};

#endif
