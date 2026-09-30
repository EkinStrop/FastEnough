#pragma once

#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

// Session objects and indices remain valid until the store is destroyed.
template<class T>
class DeviceSessionStore {
public:
    explicit DeviceSessionStore(int reserved = 2) {
        for (int i = 0; i < reserved; ++i) m_records.push_back({{}, std::make_unique<T>()});
    }

    int ensure(const std::string& identity) {
        if (identity.empty()) throw std::invalid_argument("Empty device identity");
        std::lock_guard<std::mutex> lock(m_mutex);
        for (int i = 0; i < (int)m_records.size(); ++i)
            if (m_records[i].identity == identity) return i;
        for (int i = 0; i < (int)m_records.size(); ++i) {
            if (!m_records[i].identity.empty()) continue;
            m_records[i].identity = identity;
            return i;
        }
        m_records.push_back({identity, std::make_unique<T>()});
        return (int)m_records.size() - 1;
    }

    int find(const std::string& identity) const {
        if (identity.empty()) return -1;
        std::lock_guard<std::mutex> lock(m_mutex);
        for (int i = 0; i < (int)m_records.size(); ++i)
            if (m_records[i].identity == identity) return i;
        return -1;
    }

    T& at(int slot) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return *m_records.at(slot).value;
    }

    int size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return (int)m_records.size();
    }

    std::vector<std::string> identities() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::string> result;
        for (const auto& record : m_records) result.push_back(record.identity);
        return result;
    }

private:
    struct Record { std::string identity; std::unique_ptr<T> value; };
    mutable std::mutex m_mutex;
    std::vector<Record> m_records;
};
