#pragma once
#include <cstdint>
#include <iterator>
#include <map>

class TransferCoverage {
public:
    explicit TransferCoverage(uint64_t size) : m_size(size) {}

    bool record(uint64_t offset, uint64_t length) {
        if (m_failed || m_skipped || offset > m_size || length > m_size - offset) return false;
        if (length == 0) {
            if (m_size != 0 || offset != 0) return false;
            m_emptyComplete = true;
            return true;
        }
        auto next = m_ranges.lower_bound(offset);
        if (next != m_ranges.end() && next->first == offset && next->second == offset + length) return true;
        if (next != m_ranges.end() && next->first < offset + length) return false;
        if (next != m_ranges.begin() && std::prev(next)->second > offset) return false;
        m_ranges.emplace_hint(next, offset, offset + length);
        m_successfulBytes += length;
        return true;
    }

    void fail() { m_failed = true; }
    void skip() { m_skipped = true; }
    bool failed() const { return m_failed; }
    bool skipped() const { return m_skipped; }
    bool complete() const {
        return !m_failed && !m_skipped && (m_size == 0 ? m_emptyComplete : m_successfulBytes == m_size);
    }
    uint64_t copiedBytes() const { return complete() ? m_size : 0; }

private:
    uint64_t m_size;
    uint64_t m_successfulBytes = 0;
    bool m_emptyComplete = false;
    bool m_failed = false;
    bool m_skipped = false;
    std::map<uint64_t, uint64_t> m_ranges;
};
