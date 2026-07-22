#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct BackupArchiveEntry {
    std::string name;
    uint32_t crc32 = 0;
    uint64_t size = 0;
    uint64_t localHeaderOffset = 0;
    uint64_t dataOffset = 0;
};

class BackupArchive {
public:
    static bool createStore(const std::filesystem::path& sourceRoot,
                            const std::filesystem::path& archivePath,
                            std::string& error);
    static bool createSelection(const std::filesystem::path& sourceArchive,
                                const std::vector<std::string>& prefixes,
                                const std::filesystem::path& archivePath,
                                std::string& error);

    bool open(const std::filesystem::path& archivePath, std::string& error);
    const std::vector<BackupArchiveEntry>& entries() const { return m_entries; }
    const BackupArchiveEntry* find(const std::string& name) const;
    bool readEntry(const std::string& name, std::string& data, std::string& error) const;
    bool extractPrefix(const std::string& prefix,
                       const std::filesystem::path& destinationRoot,
                       std::vector<std::filesystem::path>& extractedFiles,
                       std::string& error) const;

private:
    std::filesystem::path m_archivePath;
    std::vector<BackupArchiveEntry> m_entries;
};
