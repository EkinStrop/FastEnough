#include "backup_archive.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>

namespace {

constexpr uint32_t kLocalHeader = 0x04034b50;
constexpr uint32_t kCentralHeader = 0x02014b50;
constexpr uint32_t kZip64End = 0x06064b50;
constexpr uint32_t kZip64Locator = 0x07064b50;
constexpr uint32_t kEnd = 0x06054b50;

void put16(std::ostream& out, uint16_t value) {
    const char bytes[2] = {
        static_cast<char>(value & 0xff),
        static_cast<char>((value >> 8) & 0xff)
    };
    out.write(bytes, sizeof(bytes));
}

void put32(std::ostream& out, uint32_t value) {
    const char bytes[4] = {
        static_cast<char>(value & 0xff),
        static_cast<char>((value >> 8) & 0xff),
        static_cast<char>((value >> 16) & 0xff),
        static_cast<char>((value >> 24) & 0xff)
    };
    out.write(bytes, sizeof(bytes));
}

void put64(std::ostream& out, uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        out.put(static_cast<char>((value >> shift) & 0xff));
    }
}

uint16_t get16(const unsigned char* p) {
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(p[1] << 8);
}

uint32_t get32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t get64(const unsigned char* p) {
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        value |= static_cast<uint64_t>(*p++) << shift;
    }
    return value;
}

bool readExact(std::istream& in, void* data, size_t size) {
    in.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
    return in.good() || static_cast<size_t>(in.gcount()) == size;
}

uint32_t updateCrc32(uint32_t crc, const unsigned char* data, size_t length) {
    static std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> result{};
        for (uint32_t i = 0; i < result.size(); ++i) {
            uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1u)));
            }
            result[i] = value;
        }
        return result;
    }();
    while (length--) crc = table[(crc ^ *data++) & 0xff] ^ (crc >> 8);
    return crc;
}

std::string archiveName(const std::filesystem::path& relative) {
    const auto value = relative.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::filesystem::path archivePathPart(const std::string& value) {
    std::u8string utf8(reinterpret_cast<const char8_t*>(value.data()), value.size());
    return std::filesystem::path(utf8);
}

bool safeEntryName(const std::string& name) {
    if (name.empty() || name.front() == '/' || name.front() == '\\') return false;
    std::filesystem::path path = archivePathPart(name);
    if (path.has_root_name() || path.has_root_directory()) return false;
    for (const auto& part : path) {
        if (part == "..") return false;
    }
    return true;
}

struct PendingEntry {
    std::filesystem::path source;
    BackupArchiveEntry entry;
};

bool parseZip64Extra(const std::vector<unsigned char>& extra,
                     bool needSize, bool needCompressed, bool needOffset,
                     uint64_t& size, uint64_t& compressed, uint64_t& offset) {
    size_t position = 0;
    while (position + 4 <= extra.size()) {
        const uint16_t tag = get16(extra.data() + position);
        const uint16_t length = get16(extra.data() + position + 2);
        position += 4;
        if (position + length > extra.size()) return false;
        if (tag == 0x0001) {
            const unsigned char* value = extra.data() + position;
            size_t remaining = length;
            auto take64 = [&](uint64_t& target) {
                if (remaining < 8) return false;
                target = get64(value);
                value += 8;
                remaining -= 8;
                return true;
            };
            if (needSize && !take64(size)) return false;
            if (needCompressed && !take64(compressed)) return false;
            if (needOffset && !take64(offset)) return false;
            return true;
        }
        position += length;
    }
    return !(needSize || needCompressed || needOffset);
}

} // namespace

bool BackupArchive::createStore(const std::filesystem::path& sourceRoot,
                                const std::filesystem::path& archivePath,
                                std::string& error) {
    error.clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(sourceRoot, ec)) {
        error = "Backup staging directory is missing";
        return false;
    }

    std::vector<PendingEntry> files;
    for (std::filesystem::recursive_directory_iterator it(sourceRoot, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        PendingEntry pending;
        pending.source = it->path();
        pending.entry.name = archiveName(std::filesystem::relative(it->path(), sourceRoot, ec));
        if (ec || !safeEntryName(pending.entry.name)) {
            error = "Backup contains an invalid file path";
            return false;
        }
        pending.entry.size = std::filesystem::file_size(pending.source, ec);
        if (ec) {
            error = "Could not read backup file size: " + pending.entry.name;
            return false;
        }
        files.push_back(std::move(pending));
    }
    if (ec) {
        error = "Could not enumerate backup staging files";
        return false;
    }
    std::sort(files.begin(), files.end(), [](const PendingEntry& a, const PendingEntry& b) {
        return a.entry.name < b.entry.name;
    });

    std::filesystem::create_directories(archivePath.parent_path(), ec);
    const auto temporaryPath = archivePath.wstring() + L".tmp";
    std::ofstream out(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "Could not create backup archive";
        return false;
    }

    std::vector<char> buffer(1024 * 1024);
    for (auto& pending : files) {
        pending.entry.localHeaderOffset = static_cast<uint64_t>(out.tellp());
        put32(out, kLocalHeader);
        put16(out, 45);
        put16(out, 0x0800);
        put16(out, 0);
        put16(out, 0);
        put16(out, 0);
        put32(out, 0);
        put32(out, 0xffffffffu);
        put32(out, 0xffffffffu);
        put16(out, static_cast<uint16_t>(pending.entry.name.size()));
        put16(out, 20);
        out.write(pending.entry.name.data(), static_cast<std::streamsize>(pending.entry.name.size()));
        put16(out, 0x0001);
        put16(out, 16);
        put64(out, pending.entry.size);
        put64(out, pending.entry.size);
        pending.entry.dataOffset = static_cast<uint64_t>(out.tellp());

        std::ifstream in(pending.source, std::ios::binary);
        if (!in) {
            error = "Could not reopen backup file: " + pending.entry.name;
            out.close();
            std::filesystem::remove(temporaryPath, ec);
            return false;
        }
        uint32_t crc = 0xffffffffu;
        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = in.gcount();
            if (count > 0) {
                out.write(buffer.data(), count);
                crc = updateCrc32(crc, reinterpret_cast<const unsigned char*>(buffer.data()), static_cast<size_t>(count));
            }
        }
        if (!out || (!in.eof() && in.fail())) {
            error = "Could not write backup file: " + pending.entry.name;
            out.close();
            std::filesystem::remove(temporaryPath, ec);
            return false;
        }
        pending.entry.crc32 = crc ^ 0xffffffffu;
        const auto endPosition = out.tellp();
        out.seekp(static_cast<std::streamoff>(pending.entry.localHeaderOffset + 14));
        put32(out, pending.entry.crc32);
        out.seekp(endPosition);
        if (!out) {
            error = "Could not finalize backup file: " + pending.entry.name;
            out.close();
            std::filesystem::remove(temporaryPath, ec);
            return false;
        }
    }

    const uint64_t centralOffset = static_cast<uint64_t>(out.tellp());
    for (const auto& pending : files) {
        put32(out, kCentralHeader);
        put16(out, 45);
        put16(out, 45);
        put16(out, 0x0800);
        put16(out, 0);
        put16(out, 0);
        put16(out, 0);
        put32(out, pending.entry.crc32);
        put32(out, 0xffffffffu);
        put32(out, 0xffffffffu);
        put16(out, static_cast<uint16_t>(pending.entry.name.size()));
        put16(out, 28);
        put16(out, 0);
        put16(out, 0);
        put16(out, 0);
        put32(out, 0);
        put32(out, 0xffffffffu);
        out.write(pending.entry.name.data(), static_cast<std::streamsize>(pending.entry.name.size()));
        put16(out, 0x0001);
        put16(out, 24);
        put64(out, pending.entry.size);
        put64(out, pending.entry.size);
        put64(out, pending.entry.localHeaderOffset);
    }
    const uint64_t centralSize = static_cast<uint64_t>(out.tellp()) - centralOffset;
    const uint64_t zip64EndOffset = static_cast<uint64_t>(out.tellp());
    put32(out, kZip64End);
    put64(out, 44);
    put16(out, 45);
    put16(out, 45);
    put32(out, 0);
    put32(out, 0);
    put64(out, files.size());
    put64(out, files.size());
    put64(out, centralSize);
    put64(out, centralOffset);
    put32(out, kZip64Locator);
    put32(out, 0);
    put64(out, zip64EndOffset);
    put32(out, 1);
    put32(out, kEnd);
    put16(out, 0);
    put16(out, 0);
    put16(out, 0xffff);
    put16(out, 0xffff);
    put32(out, 0xffffffffu);
    put32(out, 0xffffffffu);
    put16(out, 0);
    out.close();
    if (!out) {
        error = "Could not finish backup archive";
        std::filesystem::remove(temporaryPath, ec);
        return false;
    }

    BackupArchive verification;
    if (!verification.open(temporaryPath, error) || !verification.find("job.json")) {
        if (error.empty()) error = "Backup archive verification failed";
        std::filesystem::remove(temporaryPath, ec);
        return false;
    }
    std::filesystem::remove(archivePath, ec);
    ec.clear();
    std::filesystem::rename(temporaryPath, archivePath, ec);
    if (ec) {
        error = "Could not finalize backup archive";
        std::filesystem::remove(temporaryPath, ec);
        return false;
    }
    return true;
}

bool BackupArchive::createSelection(const std::filesystem::path& sourceArchive,
                                    const std::vector<std::string>& prefixes,
                                    const std::filesystem::path& archivePath,
                                    std::string& error) {
    error.clear();
    BackupArchive source;
    if (!source.open(sourceArchive, error)) return false;

    std::vector<BackupArchiveEntry> selected;
    for (const auto& entry : source.entries()) {
        bool include = entry.name == "job.json";
        for (const auto& prefix : prefixes) {
            if (entry.name.compare(0, prefix.size(), prefix) == 0 &&
                (entry.name.size() == prefix.size() || prefix.back() == '/' || entry.name[prefix.size()] == '/')) {
                include = true;
                break;
            }
        }
        if (include) selected.push_back(entry);
    }
    if (selected.empty()) {
        error = "No selected app files were found in the backup archive";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(archivePath.parent_path(), ec);
    const auto temporaryPath = archivePath.wstring() + L".tmp";
    std::ifstream input(sourceArchive, std::ios::binary);
    std::ofstream out(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!input || !out) {
        error = "Could not create the selected restore archive";
        return false;
    }

    std::vector<char> buffer(1024 * 1024);
    for (auto& entry : selected) {
        entry.localHeaderOffset = static_cast<uint64_t>(out.tellp());
        put32(out, kLocalHeader);
        put16(out, 45);
        put16(out, 0x0800);
        put16(out, 0);
        put16(out, 0);
        put16(out, 0);
        put32(out, entry.crc32);
        put32(out, 0xffffffffu);
        put32(out, 0xffffffffu);
        put16(out, static_cast<uint16_t>(entry.name.size()));
        put16(out, 20);
        out.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
        put16(out, 0x0001);
        put16(out, 16);
        put64(out, entry.size);
        put64(out, entry.size);
        entry.dataOffset = static_cast<uint64_t>(out.tellp());

        input.clear();
        input.seekg(static_cast<std::streamoff>(source.find(entry.name)->dataOffset));
        uint64_t remaining = entry.size;
        while (remaining > 0) {
            const size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
            if (!readExact(input, buffer.data(), chunk)) {
                error = "The source backup archive is truncated";
                out.close();
                std::filesystem::remove(temporaryPath, ec);
                return false;
            }
            out.write(buffer.data(), static_cast<std::streamsize>(chunk));
            if (!out) {
                error = "Could not write the selected restore archive";
                out.close();
                std::filesystem::remove(temporaryPath, ec);
                return false;
            }
            remaining -= chunk;
        }
    }

    const uint64_t centralOffset = static_cast<uint64_t>(out.tellp());
    for (const auto& entry : selected) {
        put32(out, kCentralHeader);
        put16(out, 45);
        put16(out, 45);
        put16(out, 0x0800);
        put16(out, 0);
        put16(out, 0);
        put16(out, 0);
        put32(out, entry.crc32);
        put32(out, 0xffffffffu);
        put32(out, 0xffffffffu);
        put16(out, static_cast<uint16_t>(entry.name.size()));
        put16(out, 28);
        put16(out, 0);
        put16(out, 0);
        put16(out, 0);
        put32(out, 0);
        put32(out, 0xffffffffu);
        out.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
        put16(out, 0x0001);
        put16(out, 24);
        put64(out, entry.size);
        put64(out, entry.size);
        put64(out, entry.localHeaderOffset);
    }
    const uint64_t centralSize = static_cast<uint64_t>(out.tellp()) - centralOffset;
    const uint64_t zip64EndOffset = static_cast<uint64_t>(out.tellp());
    put32(out, kZip64End);
    put64(out, 44);
    put16(out, 45);
    put16(out, 45);
    put32(out, 0);
    put32(out, 0);
    put64(out, selected.size());
    put64(out, selected.size());
    put64(out, centralSize);
    put64(out, centralOffset);
    put32(out, kZip64Locator);
    put32(out, 0);
    put64(out, zip64EndOffset);
    put32(out, 1);
    put32(out, kEnd);
    put16(out, 0);
    put16(out, 0);
    put16(out, 0xffff);
    put16(out, 0xffff);
    put32(out, 0xffffffffu);
    put32(out, 0xffffffffu);
    put16(out, 0);
    out.close();
    if (!out) {
        error = "Could not finish the selected restore archive";
        std::filesystem::remove(temporaryPath, ec);
        return false;
    }
    BackupArchive verification;
    if (!verification.open(temporaryPath, error)) {
        std::filesystem::remove(temporaryPath, ec);
        return false;
    }
    std::filesystem::remove(archivePath, ec);
    ec.clear();
    std::filesystem::rename(temporaryPath, archivePath, ec);
    if (ec) {
        error = "Could not finalize the selected restore archive";
        std::filesystem::remove(temporaryPath, ec);
        return false;
    }
    return true;
}

bool BackupArchive::open(const std::filesystem::path& archivePath, std::string& error) {
    error.clear();
    m_entries.clear();
    m_archivePath.clear();
    std::ifstream in(archivePath, std::ios::binary);
    if (!in) {
        error = "Could not open backup archive";
        return false;
    }
    in.seekg(0, std::ios::end);
    const uint64_t fileSize = static_cast<uint64_t>(in.tellg());
    const uint64_t tailSize = std::min<uint64_t>(fileSize, 65557);
    std::vector<unsigned char> tail(static_cast<size_t>(tailSize));
    in.seekg(static_cast<std::streamoff>(fileSize - tailSize));
    if (!readExact(in, tail.data(), tail.size())) {
        error = "Could not read backup archive index";
        return false;
    }
    size_t endPosition = std::string::npos;
    if (tail.size() >= 22) {
        for (size_t i = tail.size() - 22;; --i) {
            if (get32(tail.data() + i) == kEnd) {
                endPosition = i;
                break;
            }
            if (i == 0) break;
        }
    }
    if (endPosition == std::string::npos) {
        error = "Backup archive index was not found";
        return false;
    }

    uint64_t entryCount = get16(tail.data() + endPosition + 10);
    uint64_t centralOffset = get32(tail.data() + endPosition + 16);
    if (entryCount == 0xffff || centralOffset == 0xffffffffu) {
        const uint64_t absoluteEnd = fileSize - tailSize + endPosition;
        if (absoluteEnd < 20) {
            error = "Backup archive ZIP64 locator is missing";
            return false;
        }
        std::array<unsigned char, 20> locator{};
        in.clear();
        in.seekg(static_cast<std::streamoff>(absoluteEnd - 20));
        if (!readExact(in, locator.data(), locator.size()) || get32(locator.data()) != kZip64Locator) {
            error = "Backup archive ZIP64 locator is invalid";
            return false;
        }
        const uint64_t zip64Offset = get64(locator.data() + 8);
        std::array<unsigned char, 56> zip64{};
        in.clear();
        in.seekg(static_cast<std::streamoff>(zip64Offset));
        if (!readExact(in, zip64.data(), zip64.size()) || get32(zip64.data()) != kZip64End) {
            error = "Backup archive ZIP64 index is invalid";
            return false;
        }
        entryCount = get64(zip64.data() + 32);
        centralOffset = get64(zip64.data() + 48);
    }
    if (entryCount > 1000000 || centralOffset >= fileSize) {
        error = "Backup archive index is out of range";
        return false;
    }

    in.clear();
    in.seekg(static_cast<std::streamoff>(centralOffset));
    m_entries.reserve(static_cast<size_t>(entryCount));
    for (uint64_t index = 0; index < entryCount; ++index) {
        std::array<unsigned char, 46> header{};
        if (!readExact(in, header.data(), header.size()) || get32(header.data()) != kCentralHeader) {
            error = "Backup archive contains an invalid entry";
            m_entries.clear();
            return false;
        }
        const uint16_t flags = get16(header.data() + 8);
        const uint16_t method = get16(header.data() + 10);
        const uint16_t nameLength = get16(header.data() + 28);
        const uint16_t extraLength = get16(header.data() + 30);
        const uint16_t commentLength = get16(header.data() + 32);
        if ((flags & 1) != 0 || method != 0) {
            error = "Backup archive contains an unsupported compressed or encrypted entry";
            m_entries.clear();
            return false;
        }
        std::string name(nameLength, '\0');
        std::vector<unsigned char> extra(extraLength);
        if (!readExact(in, name.data(), name.size()) || !readExact(in, extra.data(), extra.size())) {
            error = "Backup archive entry is truncated";
            m_entries.clear();
            return false;
        }
        in.seekg(commentLength, std::ios::cur);
        if (!in || !safeEntryName(name)) {
            error = "Backup archive contains an unsafe file path";
            m_entries.clear();
            return false;
        }
        BackupArchiveEntry entry;
        entry.name = std::move(name);
        entry.crc32 = get32(header.data() + 16);
        uint64_t compressed = get32(header.data() + 20);
        entry.size = get32(header.data() + 24);
        entry.localHeaderOffset = get32(header.data() + 42);
        const bool needCompressed = compressed == 0xffffffffu;
        const bool needSize = entry.size == 0xffffffffu;
        const bool needOffset = entry.localHeaderOffset == 0xffffffffu;
        if ((needCompressed || needSize || needOffset) &&
            !parseZip64Extra(extra, needSize, needCompressed, needOffset,
                             entry.size, compressed, entry.localHeaderOffset)) {
            error = "Backup archive ZIP64 entry is invalid";
            m_entries.clear();
            return false;
        }
        if (compressed != entry.size || entry.localHeaderOffset + 30 > fileSize) {
            error = "Backup archive entry data is invalid";
            m_entries.clear();
            return false;
        }
        const auto returnPosition = in.tellg();
        std::array<unsigned char, 30> local{};
        in.seekg(static_cast<std::streamoff>(entry.localHeaderOffset));
        if (!readExact(in, local.data(), local.size()) || get32(local.data()) != kLocalHeader) {
            error = "Backup archive local entry is invalid";
            m_entries.clear();
            return false;
        }
        entry.dataOffset = entry.localHeaderOffset + 30 + get16(local.data() + 26) + get16(local.data() + 28);
        if (entry.dataOffset > fileSize || entry.size > fileSize - entry.dataOffset) {
            error = "Backup archive entry exceeds the file size";
            m_entries.clear();
            return false;
        }
        in.seekg(returnPosition);
        m_entries.push_back(std::move(entry));
    }
    m_archivePath = archivePath;
    return true;
}

const BackupArchiveEntry* BackupArchive::find(const std::string& name) const {
    const auto it = std::find_if(m_entries.begin(), m_entries.end(), [&](const BackupArchiveEntry& entry) {
        return entry.name == name;
    });
    return it == m_entries.end() ? nullptr : &*it;
}

bool BackupArchive::readEntry(const std::string& name, std::string& data, std::string& error) const {
    error.clear();
    const auto* entry = find(name);
    if (!entry) {
        error = "Backup archive entry was not found: " + name;
        return false;
    }
    if (entry->size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        error = "Backup archive entry is too large to read into memory";
        return false;
    }
    std::ifstream in(m_archivePath, std::ios::binary);
    if (!in) {
        error = "Could not reopen backup archive";
        return false;
    }
    data.resize(static_cast<size_t>(entry->size));
    in.seekg(static_cast<std::streamoff>(entry->dataOffset));
    if (!readExact(in, data.data(), data.size())) {
        error = "Backup archive entry is truncated: " + name;
        data.clear();
        return false;
    }
    return true;
}

bool BackupArchive::extractPrefix(const std::string& prefix,
                                  const std::filesystem::path& destinationRoot,
                                  std::vector<std::filesystem::path>& extractedFiles,
                                  std::string& error) const {
    error.clear();
    extractedFiles.clear();
    if (!safeEntryName(prefix)) {
        error = "Backup archive prefix is invalid";
        return false;
    }
    std::ifstream in(m_archivePath, std::ios::binary);
    if (!in) {
        error = "Could not reopen backup archive";
        return false;
    }
    std::vector<char> buffer(1024 * 1024);
    for (const auto& entry : m_entries) {
        if (entry.name.compare(0, prefix.size(), prefix) != 0) continue;
        if (entry.name.size() > prefix.size() && prefix.back() != '/' && entry.name[prefix.size()] != '/') continue;
        std::string relativeName = entry.name.substr(prefix.size());
        while (!relativeName.empty() && relativeName.front() == '/') relativeName.erase(relativeName.begin());
        const auto relative = archivePathPart(relativeName);
        std::filesystem::path cleaned;
        for (const auto& part : relative) {
            if (part.empty() || part == ".") continue;
            if (part == "..") {
                error = "Backup archive contains an unsafe file path";
                return false;
            }
            cleaned /= part;
        }
        if (cleaned.empty()) continue;
        const auto outputPath = destinationRoot / cleaned;
        std::error_code ec;
        std::filesystem::create_directories(outputPath.parent_path(), ec);
        if (ec) {
            error = "Could not create restore staging directory";
            return false;
        }
        std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "Could not create restore staging file";
            return false;
        }
        in.clear();
        in.seekg(static_cast<std::streamoff>(entry.dataOffset));
        uint64_t remaining = entry.size;
        uint32_t crc = 0xffffffffu;
        while (remaining > 0) {
            const size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
            if (!readExact(in, buffer.data(), chunk)) {
                error = "Backup archive entry is truncated: " + entry.name;
                return false;
            }
            out.write(buffer.data(), static_cast<std::streamsize>(chunk));
            if (!out) {
                error = "Could not write restore staging file";
                return false;
            }
            crc = updateCrc32(crc, reinterpret_cast<const unsigned char*>(buffer.data()), chunk);
            remaining -= chunk;
        }
        if ((crc ^ 0xffffffffu) != entry.crc32) {
            error = "Backup archive entry failed its integrity check: " + entry.name;
            return false;
        }
        extractedFiles.push_back(outputPath);
    }
    if (extractedFiles.empty()) {
        error = "No files were found for the selected app";
        return false;
    }
    return true;
}
