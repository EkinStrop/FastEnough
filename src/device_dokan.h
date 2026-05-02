#pragma once
#include "device_client.h"
#include <string>
#include <mutex>
#include <atomic>

// Dokan virtual filesystem for Android device storage.
// Exposes device files as a local Windows drive letter.
// Reads are proxied on-demand via CMD_READ_RANGE — no local caching.

class DeviceMountManager {
public:
    // Access by device slot (0=primary, 1=secondary)
    static DeviceMountManager& instance(int slot = 0);

    // Mount device storage as a drive letter (e.g., "P:\\").
    // Runs Dokan in a background thread. Returns true on success.
    bool mount(DeviceClient* device, const std::string& storageRoot,
               const std::string& mountPoint);

    // Unmount and clean up
    void unmount();

    bool isMounted() const { return m_mounted; }
    const std::string& mountPoint() const { return m_mountPoint; }
    int slot() const { return m_slot; }

    // For health check suppression
    std::atomic<bool> ioActive{false};

private:
    DeviceMountManager() = default;
    int m_slot = 0;
    mutable std::mutex m_mutex;
    std::string m_mountPoint;
    std::atomic<bool> m_mounted{false};
    void* m_dokanInstance = nullptr; // DOKAN_HANDLE
    std::thread m_thread;
};
