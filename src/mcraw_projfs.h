#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <map>
#include <memory>
#include <functional>

// Manages ProjFS mounts for MCRAW files on Windows.
// Each mount creates a temp directory where DNG/WAV/JSON files appear on-demand.

class McrawMount {
public:
    ~McrawMount();

    // Mount a .mcraw file to a temp directory. Returns the mount path, or empty on failure.
    static std::shared_ptr<McrawMount> mount(const std::string& mcrawPath, std::string& outMountPath);

    // Unmount and clean up
    void unmount();

    const std::string& mcrawPath() const { return m_mcrawPath; }
    const std::string& mountPath() const { return m_mountPath; }

private:
    McrawMount() = default;

    std::string m_mcrawPath;
    std::string m_mountPath;
    std::wstring m_mountPathW;
    void* m_virtContext = nullptr; // PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT
    void* m_mountContext = nullptr; // MountContext* (owns the decoder/file handle)
    bool m_mounted = false;
};

// Global mount manager — tracks all active MCRAW mounts
class McrawMountManager {
public:
    static McrawMountManager& instance();

    // Mount a .mcraw and return the mount path
    std::string mountMcraw(const std::string& mcrawPath);

    // Check if a path is inside an active mount
    bool isInsideMount(const std::string& path) const;

    // Unmount all
    void unmountAll();

private:
    McrawMountManager() = default;
    mutable std::mutex m_mutex;
    std::vector<std::shared_ptr<McrawMount>> m_mounts;
};
