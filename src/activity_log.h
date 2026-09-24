#pragma once
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

enum class LogLevel { Debug, Info, Warn, Error };

struct LogEntry {
    std::chrono::steady_clock::time_point time;
    std::chrono::system_clock::time_point wallTime;
    LogLevel level;
    std::string tag;
    std::string message;
    uint64_t sequence;
    std::thread::id thread;
};

class DebugLog {
public:
    static DebugLog& instance() { static DebugLog logger; return logger; }

    static std::string format(const LogEntry& entry) {
        auto clock = std::chrono::system_clock::to_time_t(entry.wallTime);
        auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(entry.wallTime.time_since_epoch()).count()%1000;
        std::tm time{};
        localtime_s(&time, &clock);
        const char* levels[] = {"DEBUG", "INFO", "WARN", "ERROR"};
        std::ostringstream line;
        line << std::put_time(&time, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << milliseconds
             << " [" << levels[(int)entry.level] << "] [" << entry.tag << "] " << entry.message
             << " [event=" << entry.sequence << " thread=" << entry.thread << ']';
        return line.str();
    }

    void log(LogLevel level, const std::string& tag, const std::string& message) noexcept {
        try {
            std::lock_guard<std::mutex> lock(m_mutex);
            LogEntry entry{std::chrono::steady_clock::now(), std::chrono::system_clock::now(), level,
                singleLine(tag), singleLine(message), ++m_sequence, std::this_thread::get_id()};
            m_entries.push_back(entry);
            if (m_entries.size()>5000) m_entries.erase(m_entries.begin(),m_entries.begin()+1000);
            m_scrollToBottom=true;
            if (m_filePath.empty()) return;
            std::string line=format(entry)+"\n";
            if (m_fileBytes+line.size()>m_maxBytes && !rotate()) return;
            if (!m_file.is_open()) open();
            if (!m_file) return;
            m_file << line;
            m_file.flush();
            if (!m_file) m_fileError="Writing the activity log failed. Check disk space and folder permissions.";
            else { m_fileBytes+=line.size(); m_fileError.clear(); }
        } catch (...) {
            // Logging must not interrupt a file operation.
        }
    }

    bool setFilePath(const std::string& path, uintmax_t maxBytes=8*1024*1024) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_file.close();
        m_file.clear();
        m_filePath=std::filesystem::path(std::u8string(path.begin(),path.end()));
        m_maxBytes=maxBytes;
        m_fileBytes=0;
        if (path.empty()) { m_fileError.clear(); return true; }
        return open();
    }

    std::string filePath() {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto value=m_filePath.u8string();
        return std::string(value.begin(),value.end());
    }
    std::string fileError() { std::lock_guard<std::mutex> lock(m_mutex); return m_fileError; }
    void clear() { std::lock_guard<std::mutex> lock(m_mutex); m_entries.clear(); }
    std::vector<LogEntry> snapshot() { std::lock_guard<std::mutex> lock(m_mutex); return m_entries; }
    std::atomic<bool> m_scrollToBottom{false};

private:
    static std::string singleLine(const std::string& value) {
        std::string text;
        text.reserve(value.size());
        for (char c:value) {
            if (c=='\n') text+=" | ";
            else if (c=='\t') text+=' ';
            else if (c!='\r' && c!='\0') text+=c;
        }
        return text;
    }
    bool open() {
        std::error_code error;
        auto parent=m_filePath.parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent,error);
        if (error) { m_fileError="Cannot create the log folder: "+error.message(); return false; }
        m_file.clear();
        m_file.open(m_filePath,std::ios::binary|std::ios::app);
        if (!m_file) { m_fileError="Cannot open the activity log. Check folder permissions."; return false; }
        m_fileBytes=std::filesystem::file_size(m_filePath,error);
        if (error) m_fileBytes=0;
        m_fileError.clear();
        return true;
    }
    bool rotate() {
        m_file.close();
        std::error_code error;
        auto backup=[&](int number) { auto path=m_filePath; path+=L"."+std::to_wstring(number); return path; };
        std::filesystem::remove(backup(3),error);
        for (int i=2; !error && i>=0; --i) {
            auto source=i==0?m_filePath:backup(i);
            if (std::filesystem::exists(source,error)) std::filesystem::rename(source,backup(i+1),error);
        }
        if (error) { m_fileError="Cannot rotate the activity log: "+error.message(); return false; }
        m_fileBytes=0;
        return open();
    }
    std::mutex m_mutex;
    std::vector<LogEntry> m_entries;
    std::filesystem::path m_filePath;
    std::ofstream m_file;
    std::string m_fileError;
    uintmax_t m_fileBytes=0, m_maxBytes=8*1024*1024;
    uint64_t m_sequence=0;
};

#define LOG_DEBUG(tag, msg) DebugLog::instance().log(LogLevel::Debug, tag, msg)
#define LOG_INFO(tag, msg)  DebugLog::instance().log(LogLevel::Info, tag, msg)
#define LOG_WARN(tag, msg)  DebugLog::instance().log(LogLevel::Warn, tag, msg)
#define LOG_ERROR(tag, msg) DebugLog::instance().log(LogLevel::Error, tag, msg)
