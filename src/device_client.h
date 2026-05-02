#pragma once
#include "protocol.h"
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>
#include <cstdint>

struct DeviceFileEntry {
    std::string name;
    uint8_t type = 0;    // 0=file, 1=dir, 2=symlink
    uint64_t size = 0;
    int64_t mtime = 0;

    bool isDirectory() const { return type == 1; }
    bool isSymlink() const { return type == 2; }
};

struct DeviceInfo {
    std::string serial;
    std::string model;
    std::string state;
};

// Progress callback: (bytesTransferred, totalBytes) -> return false to cancel
using ProgressCallback = std::function<bool(uint64_t transferred, uint64_t total)>;

class DeviceClient {
public:
    DeviceClient();
    ~DeviceClient();

    // ADB path management
    bool findAdb();
    const std::string& getAdbPath() const { return m_adbPath; }
    void setAdbPath(const std::string& path) { m_adbPath = path; }

    // Device management (still uses ADB for device discovery)
    std::vector<DeviceInfo> getDevices();

    // ADB track-devices: persistent connection to ADB server for instant device notifications
    // Returns a connected socket, or INVALID_SOCKET on failure. Caller reads device updates.
    uintptr_t openTrackDevices();
    // Read one device update from a track-devices socket. Blocks until data arrives or error.
    // Returns parsed device list, or empty on error/disconnect.
    std::vector<DeviceInfo> readTrackDevicesUpdate(uintptr_t sock);
    void closeTrackDevices(uintptr_t sock);

    // Server lifecycle
    bool startServer(const std::string& serial, bool preferAdbForward = false);
    void stopServer();
    bool isServerRunning() const { return m_connected; }
    const std::string& connectedSerial() const { return m_serial; }
    bool isDirectConnection() const { return m_directConnection; }
    const std::string& deviceIp() const { return m_deviceIp; }

    // Try to upgrade from ADB forward to direct TCP (call anytime, even mid-session)
    bool tryUpgradeToDirect();

    // File operations via TCP server
    std::string detectStoragePath();
    std::vector<DeviceFileEntry> listDirectory(const std::string& path);
    bool pullFile(const std::string& remotePath, const std::string& localPath,
                  uint64_t& outFileSize, ProgressCallback progress = nullptr);
    bool pushFile(const std::string& localPath, const std::string& remotePath,
                  uint64_t fileSize, ProgressCallback progress = nullptr);

    // Zero-copy relay: stream file directly from src device to dst device through PC memory.
    // No disk I/O — recv chunks from src and forward them to dst in a double-buffered pipeline.
    static bool relayFile(DeviceClient& src, DeviceClient& dst,
                          const std::string& srcPath, const std::string& dstPath,
                          uint64_t fileSize, ProgressCallback progress = nullptr);
    // Resume-capable transfers: pick up where a disconnected transfer left off
    bool resumePullFile(const std::string& remotePath, const std::string& localPath,
                        uint64_t& outFileSize, ProgressCallback progress = nullptr);
    bool resumePushFile(const std::string& localPath, const std::string& remotePath,
                        uint64_t fileSize, ProgressCallback progress = nullptr);

    // Random byte-level read: fetch [offset, offset+length) from remote file into outBuffer
    // Returns actual bytes read (may be less than length at EOF), or 0 on error
    uint64_t readRange(const std::string& remotePath, uint64_t offset, uint64_t length,
                       void* outBuffer);

    // Streaming read range: fetches [offset, offset+length) and calls chunkCallback for each
    // chunk as it arrives. Callback receives (chunkData, chunkLen, chunkOffset) and returns
    // true to continue, false to abort. Returns total bytes delivered.
    using ReadRangeCallback = std::function<bool(const void* data, uint32_t len, uint64_t offset)>;
    uint64_t readRangeStreaming(const std::string& remotePath, uint64_t offset, uint64_t length,
                                ReadRangeCallback chunkCallback);

    // Get device disk space: returns true and fills total/free in bytes
    bool getDiskSpace(uint64_t& totalBytes, uint64_t& freeBytes);

    // Write bytes at offset to remote file. Returns bytes written, 0 on error.
    uint64_t writeRange(const std::string& remotePath, uint64_t offset,
                        const void* data, uint32_t length);

    // Streaming write: reads from local file at localOffset and streams to remote file at remoteOffset.
    // Single CMD_WRITE_RANGE command, streams 4MB chunks. Returns bytes written.
    uint64_t writeRangeStreaming(const std::string& remotePath, uint64_t remoteOffset,
                                const std::string& localPath, uint64_t localOffset,
                                uint64_t length, ProgressCallback progress = nullptr);

    // Create/truncate a file on the device with optional pre-allocation
    bool createFile(const std::string& remotePath, uint64_t totalSize = 0);

    bool deleteFile(const std::string& path);
    bool createDirectory(const std::string& path);
    bool renameFile(const std::string& oldPath, const std::string& newPath);
    uint64_t getFileSize(const std::string& path);

    // MCRAW virtual directory support
    std::vector<DeviceFileEntry> listMcraw(const std::string& mcrawPath);
    bool pullMcrawItem(const std::string& mcrawPath, const std::string& virtualName,
                       const std::string& localPath, uint64_t& outFileSize,
                       ProgressCallback progress = nullptr);

    // CRC32 verification - computes CRC on both sides and compares
    // Returns true if match, false if mismatch or error
    // Optional timing outputs: remoteMs/localMs report server/client CRC computation time
    bool verifyFileCrc(const std::string& remotePath, const std::string& localPath, std::string& detail,
                       std::atomic<float>* crcProgress = nullptr, std::atomic<int>* crcPhase = nullptr,
                       double* remoteMs = nullptr, double* localMs = nullptr);

    std::string lastError() const { return m_lastError; }
    std::string statusText() const { return m_statusText; } // current activity for UI display

    // Connection management (public for UI access)
    bool connectTcp(const std::string& host, int port, const std::string& bindLocalIp = "");
    void disconnectTcp();
    bool verifyConnection();
    bool connectDirect(const std::string& ip); // connect + verify + set state
    void flushStaleData(); // drain any leftover data from TCP buffer after cancelled transfer
    bool tryEnableUsbTethering(const std::string& serial = ""); // attempt to enable tethering via adb shell
    std::string detectDeviceIp(const std::string& serial);
    std::string runAdbCommand(const std::string& args);

    std::string getServerBinaryPath();

private:
    bool sendAll(const void* data, size_t len);
    bool recvAll(void* data, size_t len);
    bool sendMsg(uint32_t cmd, const void* payload = nullptr, uint32_t len = 0);
    bool recvMsg(MsgHeader& hdr, std::vector<char>& payload);

    bool tryDirectConnect(const std::string& ip);

    std::string runProcess(const std::string& command);

    std::string m_adbPath;
    std::string m_serial;
    uintptr_t m_socket = ~(uintptr_t)0; // INVALID_SOCKET
    bool m_connected = false;
    bool m_directConnection = false; // true = direct IP, false = adb forward
    std::string m_deviceIp;
    std::string m_lastError;
    std::string m_statusText;
    std::mutex m_mutex; // protects TCP socket access
    std::mutex m_serverMutex; // protects startServer/stopServer from concurrent calls

    // Reusable transfer buffer — avoids per-chunk heap allocation
    std::vector<char> m_transferBuf;

    // Inline CRC — computed during transfer to avoid re-reading the file
    uint32_t m_inlineCrc = 0;
    std::string m_inlineCrcPath; // which file this CRC belongs to
    void inlineCrcUpdate(const void* data, size_t len);
    void inlineCrcReset() { m_inlineCrc = ~(uint32_t)0; m_inlineCrcPath.clear(); }

    // Server process handle
    void* m_serverProcess = nullptr;
    int m_localPort = AFM_PORT;

public:
    void setLocalPort(int port) { m_localPort = port; }
    int localPort() const { return m_localPort; }
    void setInlineCrc(uint32_t crc, const std::string& path) { m_inlineCrc = crc; m_inlineCrcPath = path; }
    uint32_t getInlineCrc() const { return m_inlineCrc; }

    // Interrupt any blocking recv/send by setting a 1ms timeout.
    // Call from another thread to unblock transfers for fast cancellation.
    void interruptBlocking();
    void restoreTimeouts(); // restore normal socket timeouts after interruptBlocking
};

// CRC32 utilities for parallel/split-file verification
uint32_t crc32Combine(uint32_t crc1, uint32_t crc2, uint64_t len2);
void crc32Update(uint32_t& crc, const void* data, size_t len); // slicing-by-8
