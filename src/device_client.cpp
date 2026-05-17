#define _CRT_SECURE_NO_WARNINGS
#include "device_client.h"
#include "app.h" // for LOG macros
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <thread>
#include <chrono>
#include <future>
#include <cstdlib>

#pragma comment(lib, "ws2_32.lib")

// UTF-8 to wide string helper
static std::wstring toWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    std::wstring wide(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), wide.data(), len);
    return wide;
}

// UTF-8 string to std::filesystem::path (via wide string to avoid ANSI codepage issues)
static std::filesystem::path toFsPath(const std::string& utf8) {
    return std::filesystem::path(toWide(utf8));
}

static struct WsaInit {
    WsaInit() { WSADATA d; WSAStartup(MAKEWORD(2,2), &d); }
    ~WsaInit() { WSACleanup(); }
} g_wsaInit;

DeviceClient::DeviceClient() {
    // Don't call findAdb() here - it runs a process which blocks
    // ADB will be found on first use by the background poll thread
}

DeviceClient::~DeviceClient() {
    stopServer();
}

bool DeviceClient::findAdb() {
    auto tryPath = [&](const std::string& p) -> bool {
        if (std::filesystem::exists(p)) { m_adbPath = p; return true; }
        return false;
    };

    // Check next to our exe first (bundled ADB)
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
    if (tryPath((exeDir / "adb.exe").string())) return true;

    // SDK paths
    if (const char* la = std::getenv("LOCALAPPDATA"))
        if (tryPath(std::string(la) + "\\Android\\Sdk\\platform-tools\\adb.exe")) return true;
    if (const char* ah = std::getenv("ANDROID_HOME"))
        if (tryPath(std::string(ah) + "\\platform-tools\\adb.exe")) return true;
    if (const char* sr = std::getenv("ANDROID_SDK_ROOT"))
        if (tryPath(std::string(sr) + "\\platform-tools\\adb.exe")) return true;

    std::string result = runProcess("where adb.exe");
    if (!result.empty()) {
        auto pos = result.find('\n');
        std::string p = result.substr(0, pos);
        while (!p.empty() && (p.back() == '\r' || p.back() == '\n' || p.back() == ' ')) p.pop_back();
        if (std::filesystem::exists(p)) { m_adbPath = p; return true; }
    }
    return false;
}

std::string DeviceClient::runProcess(const std::string& command) {
    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE hRead, hWrite;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return "";
    STARTUPINFOA si{}; si.cb = sizeof(si);
    si.hStdOutput = hWrite; si.hStdError = hWrite;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::string cmd = command;
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hRead); CloseHandle(hWrite); return "";
    }
    CloseHandle(hWrite);
    std::string output; char buf[4096]; DWORD n;
    while (ReadFile(hRead, buf, sizeof(buf)-1, &n, nullptr) && n > 0) { buf[n] = 0; output += buf; }
    CloseHandle(hRead);
    WaitForSingleObject(pi.hProcess, 30000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return output;
}

std::string DeviceClient::runAdbCommand(const std::string& args) {
    LOG_DEBUG("ADB", "$ adb " + args);
    std::string result = runProcess("\"" + m_adbPath + "\" " + args);
    std::string trimmed = result;
    while (!trimmed.empty() && (trimmed.back() == '\r' || trimmed.back() == '\n')) trimmed.pop_back();
    if (trimmed.size() > 200) trimmed = trimmed.substr(0, 200) + "...";
    if (!trimmed.empty()) LOG_DEBUG("ADB", "  -> " + trimmed);
    return result;
}

std::vector<DeviceInfo> DeviceClient::getDevices() {
    std::vector<DeviceInfo> devices;
    if (m_adbPath.empty()) return devices;
    std::string output = runAdbCommand("devices -l");
    std::istringstream stream(output);
    std::string line;
    std::getline(stream, line); // skip header
    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        DeviceInfo d;
        std::istringstream ls(line);
        ls >> d.serial >> d.state;
        auto mp = line.find("model:");
        if (mp != std::string::npos) {
            auto end = line.find(' ', mp);
            d.model = line.substr(mp + 6, end - mp - 6);
        } else d.model = d.serial;
        if (!d.serial.empty()) devices.push_back(std::move(d));
    }
    return devices;
}

static bool adbProtoSend(SOCKET sock, const std::string& cmd) {
    char lenBuf[5];
    snprintf(lenBuf, sizeof(lenBuf), "%04x", (unsigned)cmd.size());
    std::string msg = std::string(lenBuf) + cmd;
    int total = (int)msg.size();
    const char* p = msg.c_str();
    while (total > 0) {
        int n = send(sock, p, total, 0);
        if (n <= 0) return false;
        p += n; total -= n;
    }
    return true;
}

static bool adbProtoRecvStatus(SOCKET sock) {
    char status[4];
    int n = recv(sock, status, 4, 0);
    return n == 4 && memcmp(status, "OKAY", 4) == 0;
}

static std::string adbProtoRecvPayload(SOCKET sock) {
    // Read 4-byte hex length, then payload
    char lenBuf[5] = {};
    int total = 0;
    while (total < 4) {
        int n = recv(sock, lenBuf + total, 4 - total, 0);
        if (n <= 0) return "";
        total += n;
    }
    unsigned len = 0;
    sscanf(lenBuf, "%04x", &len);
    if (len == 0) return " "; // empty device list (return non-empty to distinguish from error)
    std::string payload(len, '\0');
    total = 0;
    while ((unsigned)total < len) {
        int n = recv(sock, &payload[total], len - total, 0);
        if (n <= 0) return "";
        total += n;
    }
    return payload;
}

uintptr_t DeviceClient::openTrackDevices() {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return (uintptr_t)INVALID_SOCKET;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5037); // ADB server port
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        LOG_WARN("ADB", "track-devices: cannot connect to ADB server");
        closesocket(sock);
        return (uintptr_t)INVALID_SOCKET;
    }

    if (!adbProtoSend(sock, "host:track-devices-l")) {
        LOG_WARN("ADB", "track-devices: send failed");
        closesocket(sock);
        return (uintptr_t)INVALID_SOCKET;
    }

    if (!adbProtoRecvStatus(sock)) {
        LOG_WARN("ADB", "track-devices: server rejected");
        closesocket(sock);
        return (uintptr_t)INVALID_SOCKET;
    }

    LOG_INFO("ADB", "track-devices: connected — waiting for device events");
    return (uintptr_t)sock;
}

std::vector<DeviceInfo> DeviceClient::readTrackDevicesUpdate(uintptr_t sockHandle) {
    std::vector<DeviceInfo> devices;
    SOCKET sock = (SOCKET)sockHandle;

    std::string payload = adbProtoRecvPayload(sock);
    if (payload.empty()) {
        // Real error or timeout — caller checks WSAGetLastError
        return devices;
    }

    if (payload == " ") {
        // Empty device list (all disconnected) — return empty but set no error
        WSASetLastError(0);
        LOG_INFO("ADB", "track-devices: all devices disconnected");
        return devices;
    }

    // Parse lines: "serial\tstate\tattr1:val1 attr2:val2..."
    std::istringstream stream(payload);
    std::string line;
    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        DeviceInfo d;
        std::istringstream ls(line);
        ls >> d.serial >> d.state;
        auto mp = line.find("model:");
        if (mp != std::string::npos) {
            auto end = line.find(' ', mp);
            d.model = line.substr(mp + 6, end - mp - 6);
        } else d.model = d.serial;
        if (!d.serial.empty()) devices.push_back(std::move(d));
    }
    return devices;
}

void DeviceClient::closeTrackDevices(uintptr_t sock) {
    if (sock != (uintptr_t)INVALID_SOCKET)
        closesocket((SOCKET)sock);
}

std::string DeviceClient::getServerBinaryPath() {
    // First check next to the exe (for development)
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
    std::string beside = (exeDir / "afm-server").string();
    if (std::filesystem::exists(beside)) return beside;

    // Extract from embedded resource to temp directory
    char tempDir[MAX_PATH];
    GetTempPathA(MAX_PATH, tempDir);
    std::string extractDir = std::string(tempDir) + "FastEnough";
    CreateDirectoryA(extractDir.c_str(), nullptr);
    std::string extractPath = extractDir + "\\afm-server";

    // Check if already extracted and up to date (compare size with resource)
    HRSRC hRes = FindResource(nullptr, MAKEINTRESOURCE(200), RT_RCDATA);
    if (!hRes) return beside; // no embedded resource, fall back

    DWORD resSize = SizeofResource(nullptr, hRes);
    bool needExtract = true;
    if (std::filesystem::exists(extractPath)) {
        try {
            if (std::filesystem::file_size(extractPath) == resSize)
                needExtract = false; // same size, assume up to date
        } catch (...) {}
    }

    if (needExtract) {
        HGLOBAL hData = LoadResource(nullptr, hRes);
        if (!hData) return beside;
        void* data = LockResource(hData);
        if (!data) return beside;

        HANDLE hFile = CreateFileW(toWide(extractPath).c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(hFile, data, resSize, &written, nullptr);
            CloseHandle(hFile);
            LOG_INFO("Server", "Extracted embedded afm-server (" + std::to_string(resSize) + " bytes)");
        }
    }

    return extractPath;
}

// Detect device's network IP for direct connection (USB tethering only)
std::string DeviceClient::detectDeviceIp(const std::string& serial) {
    // Only check USB tethering interfaces — wlan0 is NOT a direct USB connection
    const char* interfaces[] = { "rndis0", "usb0", "ncm0", nullptr };

    for (int i = 0; interfaces[i]; i++) {
        std::string out = runAdbCommand("-s " + serial + " shell \"ip -4 addr show " +
                                         interfaces[i] + " 2>/dev/null\"");
        // Parse "inet X.X.X.X/YY" from output
        auto inetPos = out.find("inet ");
        if (inetPos != std::string::npos) {
            inetPos += 5;
            auto slashPos = out.find('/', inetPos);
            auto spacePos = out.find(' ', inetPos);
            auto endPos = std::min(slashPos, spacePos);
            if (endPos != std::string::npos && endPos > inetPos) {
                std::string ip = out.substr(inetPos, endPos - inetPos);
                // Validate it looks like an IP and isn't loopback
                if (ip.find('.') != std::string::npos && ip != "127.0.0.1") {
                    return ip;
                }
            }
        }
    }
    return ""; // no reachable IP found
}

bool DeviceClient::tryDirectConnect(const std::string& ip) {
    if (ip.empty()) return false;
    disconnectTcp();
    if (connectTcp(ip, AFM_PORT) && verifyConnection()) {
        m_directConnection = true;
        m_deviceIp = ip;
        return true;
    }
    disconnectTcp();
    return false;
}

bool DeviceClient::verifyConnection() {
    std::lock_guard<std::mutex> lk(m_mutex);
    // Use a short recv timeout for PING so we don't block m_mutex for 10s
    SOCKET sock = (SOCKET)m_socket;
    DWORD shortTimeout = 3000; // 3s is plenty for a PING response
    DWORD origTimeout = 10000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&shortTimeout, sizeof(shortTimeout));

    LOG_DEBUG("TCP", "Sending PING to verify connection...");
    if (!sendMsg(CMD_PING)) {
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));
        LOG_ERROR("TCP", "PING send failed: " + m_lastError); return false;
    }
    MsgHeader hdr; std::vector<char> payload;
    if (!recvMsg(hdr, payload)) {
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));
        LOG_ERROR("TCP", "PING recv failed: " + m_lastError); return false;
    }
    // Restore normal timeout
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));

    if (hdr.cmd != RSP_OK || payload.size() < 4) {
        LOG_ERROR("TCP", "PING bad response: cmd=" + std::to_string(hdr.cmd) + " len=" + std::to_string(payload.size()));
        return false;
    }
    uint32_t magic;
    memcpy(&magic, payload.data(), 4);
    if (magic != AFM_MAGIC) { LOG_ERROR("TCP", "PING wrong magic: 0x" + std::to_string(magic)); return false; }
    // Check protocol version if server provides it (backward compatible: old servers send 4 bytes)
    if (payload.size() >= 8) {
        uint32_t serverVersion;
        memcpy(&serverVersion, payload.data() + 4, 4);
        if (serverVersion != AFM_VERSION) {
            m_lastError = "Protocol version mismatch: client=" + std::to_string(AFM_VERSION) +
                          " server=" + std::to_string(serverVersion);
            LOG_ERROR("TCP", m_lastError);
            return false;
        }
    }
    LOG_INFO("TCP", "PING OK (payload " + std::to_string(payload.size()) + " bytes)");
    return true;
}

bool DeviceClient::isRootAvailable(const std::string& serial) {
    std::string out = runAdbCommand("-s " + serial + " shell \"su -c id 2>/dev/null\"");
    return out.find("uid=0") != std::string::npos;
}

// Wrap a shell command in `su -c '...'` when root mode is on. Uses single
// quotes inside the outer double-quoted adb shell string.
static std::string wrapSu(const std::string& innerCmd, bool useRoot) {
    if (!useRoot) return innerCmd;
    return "su -c '" + innerCmd + "'";
}

bool DeviceClient::startServer(const std::string& serial, bool preferAdbForward, bool useRoot) {
    std::lock_guard<std::mutex> serverLk(m_serverMutex);
    m_useRoot = useRoot;
    LOG_INFO("Server", std::string("startServer(serial=") + serial + " prefer=" + (preferAdbForward ? "ADBForward" : "DirectTCP") + " root=" + (useRoot ? "Y" : "N") + ") begin");
    if (m_connected && m_serial == serial) { LOG_INFO("Server", "Already connected to " + serial); return true; }

    // If we previously had a connection to this device that died, kill the old server
    // so it releases its dead client socket and can accept new connections
    bool wasConnectedToSameDevice = (!m_connected && m_serial == serial && !m_serial.empty());
    disconnectTcp();
    m_connected = false;
    m_directConnection = false;
    std::string savedIp = m_deviceIp; // preserve IP from tethering detection
    LOG_DEBUG("Server", "savedIp from tethering: '" + savedIp + "'");
    m_deviceIp.clear();
    m_serial = serial;

    if (wasConnectedToSameDevice) {
        LOG_INFO("Server", "Reconnecting to same device - killing old server to clear dead sockets");
        // Kill both shell-user and root server instances regardless of mode.
        runAdbCommand("-s " + serial + " shell \"killall afm-server 2>/dev/null; su -c 'killall afm-server' 2>/dev/null\"");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        // Restart server immediately
        std::string serverBin = getServerBinaryPath();
        if (std::filesystem::exists(serverBin)) {
            runAdbCommand("-s " + serial + " push \"" + serverBin + "\" /data/local/tmp/afm-server");
            runAdbCommand("-s " + serial + " shell chmod 755 /data/local/tmp/afm-server");
            std::string launchInner = wrapSu("nohup /data/local/tmp/afm-server " +
                std::to_string(AFM_PORT) + " > /dev/null 2>&1 &", m_useRoot);
            std::string cmd = "\"" + m_adbPath + "\" -s " + serial +
                " shell \"" + launchInner + "\"";
            std::string cmdBuf = cmd;
            STARTUPINFOA si2{}; si2.cb = sizeof(si2);
            si2.dwFlags = STARTF_USESHOWWINDOW; si2.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi2{};
            CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si2, &pi2);
            if (pi2.hProcess) { WaitForSingleObject(pi2.hProcess, 3000); CloseHandle(pi2.hProcess); CloseHandle(pi2.hThread); }
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            LOG_INFO("Server", "Fresh server started after reconnect");
        }
    }

    // 1. Try connecting to an already-running server
    m_statusText = "Checking for existing server...";
    std::string deviceIp = detectDeviceIp(serial);
    LOG_INFO("Server", "detectDeviceIp -> '" + deviceIp + "'");
    if (deviceIp.empty() && !savedIp.empty()) { deviceIp = savedIp; LOG_INFO("Server", "Using savedIp: " + savedIp); }
    if (!deviceIp.empty() && !preferAdbForward) {
        LOG_INFO("Server", "Step 1a: Trying existing server via Direct TCP (" + deviceIp + ")");
        m_statusText = "Trying Direct TCP (" + deviceIp + ")...";
        if (tryDirectConnect(deviceIp)) {
            LOG_INFO("Server", "Connected to existing server via Direct TCP");
            m_statusText = "Connected via Direct TCP";
            m_connected = true;
            return true;
        }
        LOG_WARN("Server", "Direct TCP to existing server failed");
    } else if (preferAdbForward) {
        LOG_INFO("Server", "Skipping Direct TCP (ADB Forward preferred)");
    } else {
        LOG_WARN("Server", "No device IP available, skipping direct TCP");
    }

    LOG_INFO("Server", "Step 1b: Trying existing server via ADB Forward");
    m_statusText = "Trying ADB Forward...";
    runAdbCommand("-s " + serial + " forward tcp:" + std::to_string(m_localPort) +
                  " tcp:" + std::to_string(AFM_PORT));
    if (connectTcp("127.0.0.1", m_localPort) && verifyConnection()) {
        LOG_INFO("Server", "Connected to existing server via ADB Forward");
        m_statusText = "Connected via ADB Forward";
        m_directConnection = false;
        m_connected = true;
        return true;
    }
    LOG_WARN("Server", "ADB Forward to existing server failed");
    disconnectTcp();

    // 2. Server not running - push and start it
    m_statusText = "Pushing server binary to device...";
    std::string serverBin = getServerBinaryPath();
    LOG_INFO("Server", "Step 2: Push server binary: " + serverBin);
    if (!std::filesystem::exists(serverBin)) {
        m_lastError = "Server binary not found: " + serverBin;
        LOG_ERROR("Server", m_lastError);
        m_statusText = "ERROR: " + m_lastError;
        return false;
    }

    std::string pushOut = runAdbCommand("-s " + serial + " push \"" + serverBin + "\" /data/local/tmp/afm-server");
    if (pushOut.find("error") != std::string::npos && pushOut.find("pushed") == std::string::npos) {
        m_lastError = "Failed to push server: " + pushOut;
        LOG_ERROR("Server", m_lastError);
        m_statusText = "ERROR: " + m_lastError;
        return false;
    }
    LOG_INFO("Server", "Push result: OK");

    LOG_INFO("Server", "Step 3: Starting server process...");
    m_statusText = "Starting server on device...";
    runAdbCommand("-s " + serial + " shell chmod 755 /data/local/tmp/afm-server");
    // Kill any prior server, whether it was started as shell or root.
    runAdbCommand("-s " + serial + " shell \"killall afm-server 2>/dev/null; su -c 'killall afm-server' 2>/dev/null\"");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Start server in background - use runProcess directly to avoid ADB shell hanging
    std::string launchInner = wrapSu("nohup /data/local/tmp/afm-server " +
        std::to_string(AFM_PORT) + " > /dev/null 2>&1 &", m_useRoot);
    std::string cmd = "\"" + m_adbPath + "\" -s " + serial +
        " shell \"" + launchInner + "\"";
    LOG_DEBUG("Server", "Launch cmd: " + cmd);
    STARTUPINFOA si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::string cmdBuf = cmd; // mutable copy for CreateProcessA
    BOOL cpOk = CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!cpOk) { LOG_ERROR("Server", "CreateProcess failed: " + std::to_string(GetLastError())); }
    if (pi.hProcess) {
        DWORD waitResult = WaitForSingleObject(pi.hProcess, 3000);
        DWORD exitCode = 0; GetExitCodeProcess(pi.hProcess, &exitCode);
        LOG_INFO("Server", "Server launch process wait=" + std::to_string(waitResult) + " exit=" + std::to_string(exitCode));
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // Verify server is actually running on the device
    std::string psCheck = runAdbCommand("-s " + serial + " shell \"pidof afm-server 2>/dev/null\"");
    bool serverPidFound = !psCheck.empty() && psCheck.find_first_of("0123456789") != std::string::npos;
    LOG_INFO("Server", "pidof afm-server: '" + psCheck + "' -> " + std::string(serverPidFound ? "FOUND" : "NOT FOUND"));

    // 3. Try direct TCP — use savedIp if detectDeviceIp fails (ADB may be flaky after tethering)
    std::string retryIp = detectDeviceIp(serial);
    LOG_INFO("Server", "Step 4: retryIp='" + retryIp + "' savedIp='" + savedIp + "' deviceIp='" + deviceIp + "'");
    if (retryIp.empty()) retryIp = savedIp;
    if (retryIp.empty()) retryIp = deviceIp;

    if (!retryIp.empty() && !preferAdbForward) {
        LOG_INFO("Server", "Step 5: Direct TCP retries to " + retryIp);
        int delayMs = 1000;
        for (int attempt = 0; attempt < 15; attempt++) {
            LOG_DEBUG("Server", "Direct TCP attempt " + std::to_string(attempt+1) + "/15 (delay=" + std::to_string(delayMs) + "ms)");
            m_statusText = "Direct TCP (" + retryIp + ") attempt " + std::to_string(attempt + 1) + "/15"
                + (serverPidFound ? "" : " [server not detected]") + " ...";
            if (tryDirectConnect(retryIp)) {
                LOG_INFO("Server", "Direct TCP connected on attempt " + std::to_string(attempt+1));
                m_statusText = "Connected via Direct TCP";
                m_connected = true;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            if (delayMs < 8000) delayMs = std::min(delayMs * 2, 8000);
            // Re-detect IP periodically in case it changed
            if (attempt % 5 == 4) {
                LOG_DEBUG("Server", "Re-detecting IP and checking server process...");
                std::string newIp = detectDeviceIp(serial);
                if (!newIp.empty() && newIp != retryIp) { LOG_INFO("Server", "IP changed: " + retryIp + " -> " + newIp); retryIp = newIp; }
                // Also re-check if server is running
                psCheck = runAdbCommand("-s " + serial + " shell \"pidof afm-server 2>/dev/null\"");
                serverPidFound = !psCheck.empty() && psCheck.find_first_of("0123456789") != std::string::npos;
                LOG_INFO("Server", std::string("pidof re-check: ") + (serverPidFound ? "FOUND" : "NOT FOUND"));
                // If server isn't running, try to start it again
                if (!serverPidFound) {
                    LOG_WARN("Server", "Server not running - restarting...");
                    m_statusText = "Server not running - restarting...";
                    std::string restartCmd = "\"" + m_adbPath + "\" -s " + serial +
                        " shell \"nohup /data/local/tmp/afm-server " + std::to_string(AFM_PORT) + " > /dev/null 2>&1 &\"";
                    std::string restartBuf = restartCmd;
                    STARTUPINFOA rsi{}; rsi.cb = sizeof(rsi);
                    rsi.dwFlags = STARTF_USESHOWWINDOW; rsi.wShowWindow = SW_HIDE;
                    PROCESS_INFORMATION rpi{};
                    CreateProcessA(nullptr, restartBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &rsi, &rpi);
                    if (rpi.hProcess) { WaitForSingleObject(rpi.hProcess, 3000); CloseHandle(rpi.hProcess); CloseHandle(rpi.hThread); }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                }
            }
        }
        LOG_WARN("Server", "All 15 Direct TCP attempts failed");
    } else {
        LOG_WARN("Server", "No IP for Direct TCP retries, skipping");
    }

    // 4. ADB forward (reliable fallback)
    LOG_INFO("Server", "Step 6: ADB Forward fallback");
    m_statusText = "Setting up ADB Forward...";
    runAdbCommand("-s " + serial + " forward tcp:" + std::to_string(m_localPort) +
                  " tcp:" + std::to_string(AFM_PORT));

    for (int attempt = 0; attempt < 5; attempt++) {
        LOG_DEBUG("Server", "ADB Forward attempt " + std::to_string(attempt+1) + "/5");
        m_statusText = "ADB Forward attempt " + std::to_string(attempt + 1) + "/5...";
        if (connectTcp("127.0.0.1", m_localPort) && verifyConnection()) {
            LOG_INFO("Server", "Connected via ADB Forward on attempt " + std::to_string(attempt+1));
            m_directConnection = false;
            m_connected = true;
            m_statusText = "Connected via ADB Forward";
            return true;
        }
        disconnectTcp();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    m_lastError = "Failed to connect to server after all attempts";
    LOG_ERROR("Server", m_lastError);
    m_statusText = "ERROR: " + m_lastError;
    return false;
}

bool DeviceClient::connectDirect(const std::string& ip) {
    disconnectTcp();
    if (connectTcp(ip, AFM_PORT) && verifyConnection()) {
        m_connected = true;
        m_directConnection = true;
        m_deviceIp = ip;
        // Remove adb forward if it was set
        if (!m_serial.empty())
            runAdbCommand("-s " + m_serial + " forward --remove tcp:" + std::to_string(m_localPort));
        return true;
    }
    disconnectTcp();
    return false;
}

bool DeviceClient::tryEnableUsbTethering(const std::string& serial) {
    std::string ser = serial.empty() ? m_serial : serial;
    if (ser.empty()) return false;
    LOG_INFO("Tether", "tryEnableUsbTethering(serial=" + ser + ")");
    m_statusText = "Enabling USB tethering...";

    // Enabling rndis changes USB function which causes ADB to disconnect/reconnect
    runAdbCommand("-s " + ser + " shell svc usb setFunctions rndis");

    // Wait for ADB to reconnect after USB mode change
    m_statusText = "Waiting for USB to settle...";
    for (int i = 0; i < 15; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto devices = getDevices();
        for (auto& d : devices) {
            if (d.state == "device") {
                // A device is back (serial might change after USB mode switch)
                std::this_thread::sleep_for(std::chrono::seconds(2));
                // Check all USB tethering interfaces — different devices use different names
                const char* tetherIfaces[] = { "rndis0", "usb0", "ncm0", nullptr };
                for (int j = 0; tetherIfaces[j]; j++) {
                    std::string out = runAdbCommand("-s " + d.serial +
                        " shell \"ip -4 addr show " + tetherIfaces[j] + " 2>/dev/null | grep inet\"");
                    if (out.find("inet ") != std::string::npos) {
                        // Parse and save the IP now while ADB still works
                        auto inetPos = out.find("inet ") + 5;
                        auto slashPos = out.find('/', inetPos);
                        auto spacePos = out.find(' ', inetPos);
                        auto endPos = std::min(slashPos, spacePos);
                        if (endPos != std::string::npos && endPos > inetPos)
                            m_deviceIp = out.substr(inetPos, endPos - inetPos);
                        LOG_INFO("Tether", std::string("Tethering enabled! ") + tetherIfaces[j] + " IP: " + m_deviceIp + " (device serial: " + d.serial + ")");
                        m_statusText = "USB tethering enabled";
                        return true;
                    }
                }
                LOG_DEBUG("Tether", "Device " + d.serial + " found but no USB tethering IP yet (attempt " + std::to_string(i) + "/15)");
            }
        }
    }

    LOG_WARN("Tether", "Tethering not available after 15 attempts");
    m_statusText = "Tethering not available";
    return false;
}

bool DeviceClient::tryUpgradeToDirect() {
    if (!m_connected || m_serial.empty() || m_directConnection) return m_directConnection;

    std::string ip = detectDeviceIp(m_serial);
    if (ip.empty()) return false;

    // Try connecting directly to the device IP
    SOCKET testSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (testSock == INVALID_SOCKET) return false;

    DWORD timeout = 1500;
    setsockopt(testSock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(AFM_PORT);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    if (connect(testSock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        closesocket(testSock);
        return false;
    }

    // Verify with PING on the new socket
    int bufsize2 = 4 * 1024 * 1024;
    setsockopt(testSock, SOL_SOCKET, SO_SNDBUF, (const char*)&bufsize2, sizeof(bufsize2));
    setsockopt(testSock, SOL_SOCKET, SO_RCVBUF, (const char*)&bufsize2, sizeof(bufsize2));
    BOOL nd = TRUE;
    setsockopt(testSock, IPPROTO_TCP, TCP_NODELAY, (const char*)&nd, sizeof(nd));

    MsgHeader pingHdr = { CMD_PING, 0 };
    if (send(testSock, (const char*)&pingHdr, sizeof(pingHdr), 0) != sizeof(pingHdr)) {
        closesocket(testSock);
        return false;
    }

    MsgHeader respHdr;
    char respBuf[64];
    int n = recv(testSock, (char*)&respHdr, sizeof(respHdr), 0);
    if (n != sizeof(respHdr) || respHdr.cmd != RSP_OK || respHdr.length < 4) {
        closesocket(testSock);
        return false;
    }
    // Recv loop to handle potential TCP fragmentation
    {
        uint32_t toRecv = respHdr.length;
        if (toRecv > sizeof(respBuf)) { closesocket(testSock); return false; }
        int totalRecv = 0;
        while ((uint32_t)totalRecv < toRecv) {
            n = recv(testSock, respBuf + totalRecv, toRecv - totalRecv, 0);
            if (n <= 0) { closesocket(testSock); return false; }
            totalRecv += n;
        }
        if (totalRecv < 4) { closesocket(testSock); return false; }
    }

    uint32_t magic;
    memcpy(&magic, respBuf, 4);
    if (magic != AFM_MAGIC) { closesocket(testSock); return false; }

    // Success - swap the socket
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        // Close old connection (adb forward)
        if (m_socket != (uintptr_t)INVALID_SOCKET)
            closesocket((SOCKET)m_socket);
        m_socket = (uintptr_t)testSock;
        m_directConnection = true;
        m_deviceIp = ip;
    }

    // Remove adb forward since we don't need it anymore
    runAdbCommand("-s " + m_serial + " forward --remove tcp:" + std::to_string(m_localPort));

    return true;
}

void DeviceClient::stopServer() {
    std::lock_guard<std::mutex> serverLk(m_serverMutex);
    if (m_connected) {
        std::lock_guard<std::mutex> lk(m_mutex);
        sendMsg(CMD_QUIT);
        MsgHeader hdr; std::vector<char> payload;
        recvMsg(hdr, payload);
    }
    disconnectTcp();

    if (!m_serial.empty() && !m_adbPath.empty()) {
        if (!m_directConnection)
            runAdbCommand("-s " + m_serial + " forward --remove tcp:" + std::to_string(m_localPort));
        runAdbCommand("-s " + m_serial + " shell killall afm-server 2>/dev/null");
    }

    m_connected = false;
    m_directConnection = false;
    m_deviceIp.clear();
    m_serial.clear();
}

uintptr_t DeviceClient::openServerSocket(const std::string& host, int port,
                                         const std::string& bindLocalIp) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        LOG_ERROR("TCP", "socket() failed: WSA " + std::to_string(WSAGetLastError()));
        return (uintptr_t)INVALID_SOCKET;
    }
    int bufsize = 4 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char*)&bufsize, sizeof(bufsize));
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char*)&bufsize, sizeof(bufsize));
    BOOL nodelay = TRUE;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));

    if (!bindLocalIp.empty()) {
        sockaddr_in bindAddr{};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_port = 0;
        inet_pton(AF_INET, bindLocalIp.c_str(), &bindAddr.sin_addr);
        if (bind(sock, (sockaddr*)&bindAddr, sizeof(bindAddr)) != 0) {
            closesocket(sock);
            return (uintptr_t)INVALID_SOCKET;
        }
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    u_long nonBlocking = 1;
    ioctlsocket(sock, FIONBIO, &nonBlocking);
    int ret = connect(sock, (sockaddr*)&addr, sizeof(addr));
    if (ret != 0) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) { closesocket(sock); return (uintptr_t)INVALID_SOCKET; }
        fd_set writeSet, exceptSet;
        FD_ZERO(&writeSet); FD_SET(sock, &writeSet);
        FD_ZERO(&exceptSet); FD_SET(sock, &exceptSet);
        timeval tv = { 5, 0 };
        int selRet = select(0, nullptr, &writeSet, &exceptSet, &tv);
        if (selRet <= 0 || FD_ISSET(sock, &exceptSet)) {
            closesocket(sock); return (uintptr_t)INVALID_SOCKET;
        }
        int sockErr = 0; int optLen = sizeof(sockErr);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&sockErr, &optLen);
        if (sockErr != 0) { closesocket(sock); return (uintptr_t)INVALID_SOCKET; }
    }
    u_long blocking = 0;
    ioctlsocket(sock, FIONBIO, &blocking);

    DWORD sndTimeout = 30000;
    DWORD rcvTimeout = 10000;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sndTimeout, sizeof(sndTimeout));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcvTimeout, sizeof(rcvTimeout));

    BOOL keepAlive = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (const char*)&keepAlive, sizeof(keepAlive));
    DWORD keepIdle = 5000, keepInterval = 1000, keepCount = 3;
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, (const char*)&keepIdle, sizeof(keepIdle));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, (const char*)&keepInterval, sizeof(keepInterval));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, (const char*)&keepCount, sizeof(keepCount));

    return (uintptr_t)sock;
}

bool DeviceClient::connectTcp(const std::string& host, int port, const std::string& bindLocalIp) {
    LOG_DEBUG("TCP", "Connecting to " + host + ":" + std::to_string(port) + "...");
    uintptr_t sockU = openServerSocket(host, port, bindLocalIp);
    if (sockU == (uintptr_t)INVALID_SOCKET) {
        LOG_WARN("TCP", "openServerSocket failed for " + host + ":" + std::to_string(port));
        return false;
    }
    LOG_INFO("TCP", "Connected to " + host + ":" + std::to_string(port));
    m_connected = true;
    m_socket = sockU;
    m_lastConnectHost = host;
    m_lastConnectPort = port;
    m_lastConnectBindIp = bindLocalIp;
    return true;
}

// Static send/recv helpers operating on an arbitrary socket.
bool DeviceClient::sendAllOn(uintptr_t sockU, const void* data, size_t len) {
    SOCKET sock = (SOCKET)sockU;
    if (sock == INVALID_SOCKET) return false;
    const char* p = (const char*)data;
    while (len > 0) {
        int n = send(sock, p, (int)std::min(len, (size_t)INT_MAX), 0);
        if (n <= 0) return false;
        p += n; len -= n;
    }
    return true;
}
bool DeviceClient::recvAllOn(uintptr_t sockU, void* data, size_t len) {
    SOCKET sock = (SOCKET)sockU;
    if (sock == INVALID_SOCKET) return false;
    char* p = (char*)data;
    while (len > 0) {
        int n = recv(sock, p, (int)std::min(len, (size_t)INT_MAX), 0);
        if (n <= 0) return false;
        p += n; len -= n;
    }
    return true;
}
bool DeviceClient::sendMsgOn(uintptr_t sock, uint32_t cmd, const void* payload, uint32_t len) {
    MsgHeader hdr = { cmd, len };
    if (!sendAllOn(sock, &hdr, sizeof(hdr))) return false;
    if (len > 0 && !sendAllOn(sock, payload, len)) return false;
    return true;
}
bool DeviceClient::recvMsgOn(uintptr_t sock, MsgHeader& hdr, std::vector<char>& payload) {
    if (!recvAllOn(sock, &hdr, sizeof(hdr))) return false;
    const uint32_t maxPayload = AFM_CHUNK_SIZE + 65536;
    if (hdr.length > maxPayload) return false;
    payload.resize(hdr.length);
    if (hdr.length > 0 && !recvAllOn(sock, payload.data(), hdr.length)) return false;
    return true;
}

bool DeviceClient::ensureControlChannelLocked() {
    if (m_ctlConnected && m_ctlSocket != (uintptr_t)INVALID_SOCKET) return true;
    if (!m_connected || m_lastConnectHost.empty()) return false;
    uintptr_t sockU = openServerSocket(m_lastConnectHost, m_lastConnectPort, m_lastConnectBindIp);
    if (sockU == (uintptr_t)INVALID_SOCKET) return false;
    m_ctlSocket = sockU;
    m_ctlConnected = true;
    LOG_INFO("TCP", "Control channel connected");
    return true;
}

void DeviceClient::closeControlChannelLocked() {
    if (m_ctlSocket != (uintptr_t)INVALID_SOCKET) {
        closesocket((SOCKET)m_ctlSocket);
        m_ctlSocket = (uintptr_t)INVALID_SOCKET;
    }
    m_ctlConnected = false;
}

bool DeviceClient::runSmallOp(uint32_t cmd, const void* payload, uint32_t plen,
                              MsgHeader& outHdr, std::vector<char>& outPayload,
                              uint32_t recvTimeoutMs) {
    // Try control channel first.
    {
        std::lock_guard<std::mutex> lk(m_ctlMutex);
        if (ensureControlChannelLocked()) {
            SOCKET sock = (SOCKET)m_ctlSocket;
            DWORD origTimeout = 10000;
            if (recvTimeoutMs) {
                DWORD t = recvTimeoutMs;
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
            }
            bool ok = sendMsgOn(m_ctlSocket, cmd, payload, plen) &&
                      recvMsgOn(m_ctlSocket, outHdr, outPayload);
            if (recvTimeoutMs) {
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));
            }
            if (ok) return true;
            // Channel died — close it; we'll fall back to primary.
            closeControlChannelLocked();
        }
    }
    // Fall back to primary channel.
    std::lock_guard<std::mutex> lk(m_mutex);
    SOCKET sock = (SOCKET)m_socket;
    DWORD origTimeout = 10000;
    if (recvTimeoutMs) {
        DWORD t = recvTimeoutMs;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
    }
    bool ok = sendMsg(cmd, payload, plen) && recvMsg(outHdr, outPayload);
    if (recvTimeoutMs) {
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));
    }
    return ok;
}

void DeviceClient::flushStaleData() {
    if (!m_connected) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    SOCKET sock = (SOCKET)m_socket;
    // Temporarily set a very short recv timeout to drain stale bytes
    DWORD shortTimeout = 100; // 100ms
    DWORD origTimeout = 10000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&shortTimeout, sizeof(shortTimeout));
    char discard[4096];
    int totalFlushed = 0;
    while (true) {
        int n = recv(sock, discard, sizeof(discard), 0);
        if (n <= 0) break;
        totalFlushed += n;
        if (totalFlushed > 1024 * 1024) break; // safety cap
    }
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));
    if (totalFlushed > 0)
        LOG_INFO("TCP", "Flushed " + std::to_string(totalFlushed) + " bytes of stale data");
}

void DeviceClient::interruptBlocking() {
    SOCKET sock = (SOCKET)m_socket;
    if (sock != INVALID_SOCKET) {
        DWORD timeout = 1;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
    }
}

void DeviceClient::restoreTimeouts() {
    SOCKET sock = (SOCKET)m_socket;
    if (sock != INVALID_SOCKET) {
        DWORD timeout = 10000; // 10s default
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
    }
}

void DeviceClient::disconnectTcp() {
    if (m_socket != (uintptr_t)INVALID_SOCKET) {
        closesocket((SOCKET)m_socket);
        m_socket = (uintptr_t)INVALID_SOCKET;
    }
    m_connected = false;
    m_directConnection = false;
    {
        std::lock_guard<std::mutex> lk(m_ctlMutex);
        closeControlChannelLocked();
    }
}

bool DeviceClient::sendAll(const void* data, size_t len) {
    if (!m_connected) { m_lastError = "Not connected"; return false; }
    const char* p = (const char*)data;
    SOCKET sock = (SOCKET)m_socket;
    while (len > 0) {
        int n = send(sock, p, (int)std::min(len, (size_t)INT_MAX), 0);
        if (n <= 0) {
            int wsaErr = WSAGetLastError();
            m_lastError = "Send failed (WSA error " + std::to_string(wsaErr) + ")";
            LOG_ERROR("TCP", m_lastError);
            m_connected = false;
            return false;
        }
        p += n; len -= n;
    }
    return true;
}

bool DeviceClient::recvAll(void* data, size_t len) {
    if (!m_connected) { m_lastError = "Not connected"; return false; }
    char* p = (char*)data;
    SOCKET sock = (SOCKET)m_socket;
    while (len > 0) {
        int n = recv(sock, p, (int)std::min(len, (size_t)INT_MAX), 0);
        if (n <= 0) {
            int wsaErr = WSAGetLastError();
            m_lastError = "Recv failed (WSA error " + std::to_string(wsaErr) + ")";
            LOG_ERROR("TCP", m_lastError);
            m_connected = false;
            return false;
        }
        p += n; len -= n;
    }
    return true;
}

bool DeviceClient::sendMsg(uint32_t cmd, const void* payload, uint32_t len) {
    MsgHeader hdr = { cmd, len };
    if (!sendAll(&hdr, sizeof(hdr))) return false;
    if (len > 0 && !sendAll(payload, len)) return false;
    return true;
}

bool DeviceClient::recvMsg(MsgHeader& hdr, std::vector<char>& payload) {
    if (!recvAll(&hdr, sizeof(hdr))) return false;
    // Sanity check: reject absurd payload sizes (max ~8MB: chunk + headers + path)
    const uint32_t maxPayload = AFM_CHUNK_SIZE + 65536;
    if (hdr.length > maxPayload) {
        m_lastError = "Protocol error: payload too large (" + std::to_string(hdr.length) + " bytes)";
        m_connected = false;
        return false;
    }
    payload.resize(hdr.length);
    if (hdr.length > 0 && !recvAll(payload.data(), hdr.length)) return false;
    return true;
}

std::string DeviceClient::detectStoragePath() {
    if (!m_connected) return "/storage/emulated/0";
    MsgHeader hdr; std::vector<char> payload;
    if (!runSmallOp(CMD_STORAGE, nullptr, 0, hdr, payload)) return "/storage/emulated/0";
    if (hdr.cmd == RSP_OK && !payload.empty())
        return std::string(payload.data(), payload.size());
    return "/storage/emulated/0";
}

std::vector<DeviceFileEntry> DeviceClient::listDirectory(const std::string& path) {
    std::vector<DeviceFileEntry> entries;
    if (!m_connected) return entries;

    MsgHeader hdr; std::vector<char> payload;
    if (!runSmallOp(CMD_LIST, path.data(), (uint32_t)path.size(), hdr, payload)) return entries;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(payload.data(), payload.size());
        return entries;
    }
    if (hdr.cmd != RSP_OK || payload.size() < 4) return entries;

    uint32_t count;
    memcpy(&count, payload.data(), 4);

    size_t offset = 4;
    for (uint32_t i = 0; i < count && offset < payload.size(); i++) {
        if (offset + sizeof(FileEntryHeader) > payload.size()) break;
        FileEntryHeader feh;
        memcpy(&feh, payload.data() + offset, sizeof(feh));
        offset += sizeof(feh);
        if (offset + feh.name_len > payload.size()) break;

        DeviceFileEntry e;
        e.type = feh.type;
        e.size = feh.size;
        e.mtime = feh.mtime;
        e.name = std::string(payload.data() + offset, feh.name_len);
        offset += feh.name_len;
        entries.push_back(std::move(e));
    }

    // Sort: dirs first, then by name
    std::sort(entries.begin(), entries.end(), [](const DeviceFileEntry& a, const DeviceFileEntry& b) {
        if (a.isDirectory() != b.isDirectory()) return a.isDirectory();
        return a.name < b.name;
    });

    return entries;
}

std::vector<DeviceFileEntry> DeviceClient::listMcraw(const std::string& mcrawPath) {
    std::vector<DeviceFileEntry> entries;
    if (!m_connected) return entries;

    MsgHeader hdr; std::vector<char> payload;
    if (!runSmallOp(CMD_MCRAW_LIST, mcrawPath.data(), (uint32_t)mcrawPath.size(),
                    hdr, payload, 30000)) {
        LOG_ERROR("MCRAW", "listMcraw failed");
        return entries;
    }
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(payload.data(), payload.size());
        return entries;
    }
    if (hdr.cmd != RSP_OK || payload.size() < 4) return entries;

    // Parse identically to listDirectory — same wire format
    uint32_t count;
    memcpy(&count, payload.data(), 4);

    size_t offset = 4;
    for (uint32_t i = 0; i < count && offset < payload.size(); i++) {
        if (offset + sizeof(FileEntryHeader) > payload.size()) break;
        FileEntryHeader feh;
        memcpy(&feh, payload.data() + offset, sizeof(feh));
        offset += sizeof(feh);
        if (offset + feh.name_len > payload.size()) break;

        DeviceFileEntry e;
        e.type = feh.type;
        e.size = feh.size;
        e.mtime = feh.mtime;
        e.name = std::string(payload.data() + offset, feh.name_len);
        offset += feh.name_len;
        entries.push_back(std::move(e));
    }

    // No sorting needed — server returns in logical order (metadata, frames, audio)
    return entries;
}

bool DeviceClient::pullMcrawItem(const std::string& mcrawPath, const std::string& virtualName,
                                  const std::string& localPath, uint64_t& outFileSize,
                                  ProgressCallback progress) {
    if (!m_connected) { m_lastError = "Not connected"; return false; }
    std::lock_guard<std::mutex> lk(m_mutex);

    // Use a longer recv timeout — server needs to decompress frame and generate DNG
    SOCKET sock = (SOCKET)m_socket;
    DWORD longTimeout = 60000; // 60s for DNG generation
    DWORD origTimeout = 10000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&longTimeout, sizeof(longTimeout));

    // Build payload: [4B mcraw_path_len][mcraw_path][virtual_name]
    uint32_t mpLen = (uint32_t)mcrawPath.size();
    std::vector<char> extractPayload(4 + mpLen + virtualName.size());
    memcpy(extractPayload.data(), &mpLen, 4);
    memcpy(extractPayload.data() + 4, mcrawPath.data(), mpLen);
    memcpy(extractPayload.data() + 4 + mpLen, virtualName.data(), virtualName.size());

    if (!sendMsg(CMD_MCRAW_EXTRACT, extractPayload.data(), (uint32_t)extractPayload.size())) {
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));
        return false;
    }

    // Receive response — identical to pullFile() from here on
    MsgHeader hdr; std::vector<char> payload;
    if (!recvMsg(hdr, payload)) {
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));
        LOG_ERROR("MCRAW", "pullMcrawItem recv failed, disconnecting to prevent desync");
        m_connected = false;
        closesocket(sock);
        m_socket = INVALID_SOCKET;
        return false;
    }
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(payload.data(), payload.size());
        return false;
    }
    if (hdr.cmd != RSP_OK || payload.size() < sizeof(PullHeader)) return false;

    PullHeader ph;
    memcpy(&ph, payload.data(), sizeof(ph));
    outFileSize = ph.file_size;

    HANDLE hFile = CreateFileW(toWide(localPath).c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        m_lastError = "Failed to create local file: " + localPath;
        // Drain server data to keep TCP stream in sync
        while (true) {
            if (!recvAll(&hdr, sizeof(hdr))) break;
            if (hdr.cmd == RSP_DONE) break;
            if (hdr.cmd != RSP_DATA || hdr.length == 0) break;
            if (m_transferBuf.size() < hdr.length) m_transferBuf.resize(hdr.length);
            if (!recvAll(m_transferBuf.data(), hdr.length)) break;
        }
        return false;
    }

    uint64_t received = 0;
    bool success = true;

    // Double-buffered receive + write (same as pullFile but without inline CRC)
    std::vector<char> bufA(AFM_CHUNK_SIZE), bufB(AFM_CHUNK_SIZE);
    char* recvBuf = bufA.data();
    std::future<bool> writeFut;

    while (received < outFileSize && success) {
        if (!recvAll(&hdr, sizeof(hdr))) { success = false; break; }
        if (hdr.cmd == RSP_DONE) break;
        if (hdr.cmd != RSP_DATA) { success = false; break; }
        if (!recvAll(recvBuf, hdr.length)) { success = false; break; }

        if (writeFut.valid()) {
            if (!writeFut.get()) success = false;
        }

        auto writeBuf = recvBuf;
        auto writeLen = hdr.length;
        writeFut = std::async(std::launch::async, [hFile, writeBuf, writeLen]() {
            DWORD written;
            return WriteFile(hFile, writeBuf, writeLen, &written, nullptr) && written == writeLen;
        });

        recvBuf = (recvBuf == bufA.data()) ? bufB.data() : bufA.data();
        received += hdr.length;

        if (progress && !progress(received, outFileSize)) {
            success = false;
            m_lastError = "Cancelled";
            break;
        }
    }

    if (writeFut.valid()) {
        if (!writeFut.get()) success = false;
    }

    CloseHandle(hFile);

    // ALWAYS drain remaining data until RSP_DONE to keep stream clean.
    // The data loop exits when received >= outFileSize, but the server still
    // sends RSP_DONE after all data. If we don't consume it, it poisons the
    // next command on this socket.
    if (m_connected) {
        while (recvAll(&hdr, sizeof(hdr))) {
            if (hdr.cmd == RSP_DONE) break;
            if (hdr.cmd != RSP_DATA || hdr.length == 0) break;
            if (m_transferBuf.size() < hdr.length) m_transferBuf.resize(hdr.length);
            if (!recvAll(m_transferBuf.data(), hdr.length)) break;
        }
    }

    return success;
}

bool DeviceClient::pullFile(const std::string& remotePath, const std::string& localPath,
                            uint64_t& outFileSize, ProgressCallback progress) {
    if (!m_connected) { m_lastError = "Not connected"; LOG_ERROR("Transfer", "pullFile: not connected"); return false; }
    LOG_INFO("Transfer", "pullFile START: " + remotePath + " -> " + localPath);
    std::lock_guard<std::mutex> lk(m_mutex);

    if (!sendMsg(CMD_PULL, remotePath.data(), (uint32_t)remotePath.size())) {
        LOG_ERROR("Transfer", "pullFile: sendMsg CMD_PULL failed");
        return false;
    }

    MsgHeader hdr; std::vector<char> payload;
    if (!recvMsg(hdr, payload)) return false;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(payload.data(), payload.size());
        return false;
    }
    if (hdr.cmd != RSP_OK || payload.size() < sizeof(PullHeader)) return false;

    PullHeader ph;
    memcpy(&ph, payload.data(), sizeof(ph));
    outFileSize = ph.file_size;

    // Open local file for writing
    HANDLE hFile = CreateFileW(toWide(localPath).c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        // Parent directory may not exist yet (race with parallel directory creation)
        try { std::filesystem::create_directories(std::filesystem::path(toFsPath(localPath)).parent_path()); } catch (...) {}
        hFile = CreateFileW(toWide(localPath).c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    }
    if (hFile == INVALID_HANDLE_VALUE) {
        m_lastError = "Failed to create local file: " + localPath;
        return false;
    }

    uint64_t received = 0;
    bool success = true;
    inlineCrcReset();

    // Double-buffered pull: async write+CRC of previous chunk while recv blocks on next
    size_t pullBufSize = AFM_CHUNK_SIZE;
    std::vector<char> bufA(pullBufSize), bufB(pullBufSize);
    char* recvBuf = bufA.data();
    std::future<bool> writeFut;

    bool gotDone = false;
    while (true) {
        if (!recvAll(&hdr, sizeof(hdr))) { success = false; break; }

        if (hdr.cmd == RSP_DONE) { gotDone = true; break; }
        if (hdr.cmd != RSP_DATA || hdr.length == 0) { success = false; break; }

        if (!recvAll(recvBuf, hdr.length)) { success = false; break; }

        // Wait for previous write+CRC to finish before swapping buffers
        if (writeFut.valid()) {
            if (!writeFut.get()) { success = false; break; }
        }

        if (progress && received > 0 && !progress(received, outFileSize)) {
            success = false;
            m_lastError = "Cancelled";
            break;
        }

        // Launch async write+CRC for current chunk
        char* writeBuf = recvBuf;
        DWORD writeLen = hdr.length;
        writeFut = std::async(std::launch::async, [this, writeBuf, writeLen, hFile, &received]() {
            DWORD written;
            if (!WriteFile(hFile, writeBuf, writeLen, &written, nullptr)) return false;
            inlineCrcUpdate(writeBuf, writeLen);
            received += written;
            return true;
        });

        recvBuf = (recvBuf == bufA.data()) ? bufB.data() : bufA.data();
    }

    // Wait for last write+CRC
    if (writeFut.valid()) {
        if (!writeFut.get()) success = false;
    }
    if (success && progress) progress(received, outFileSize);

    CloseHandle(hFile);

    // Drain until RSP_DONE only if the main loop didn't already consume it.
    // Without this, the drain loop would block on recv until timeout, then
    // set m_connected=false — killing the connection after a successful pull.
    if (m_connected && !gotDone) {
        MsgHeader drainHdr;
        while (recvAll(&drainHdr, sizeof(drainHdr))) {
            if (drainHdr.cmd == RSP_DONE) break;
            if (drainHdr.cmd != RSP_DATA || drainHdr.length == 0) break;
            if (m_transferBuf.size() < drainHdr.length) m_transferBuf.resize(drainHdr.length);
            if (!recvAll(m_transferBuf.data(), drainHdr.length)) break;
        }
    }

    if (success) {
        m_inlineCrc = ~m_inlineCrc;
        m_inlineCrcPath = localPath;
        LOG_INFO("Transfer", "pullFile OK: " + std::to_string(received) + " bytes received");
    } else {
        LOG_ERROR("Transfer", "pullFile FAILED: " + m_lastError);
    }
    return success;
}

bool DeviceClient::pushFile(const std::string& localPath, const std::string& remotePath,
                            uint64_t fileSize, ProgressCallback progress) {
    if (!m_connected) { m_lastError = "Not connected"; return false; }
    std::lock_guard<std::mutex> lk(m_mutex);

    // Build push header: [8B size][path]
    std::vector<char> pushPayload(sizeof(PushHeader) + remotePath.size());
    PushHeader ph = { fileSize };
    memcpy(pushPayload.data(), &ph, sizeof(ph));
    memcpy(pushPayload.data() + sizeof(ph), remotePath.data(), remotePath.size());

    if (!sendMsg(CMD_PUSH, pushPayload.data(), (uint32_t)pushPayload.size())) return false;

    // Wait for RSP_OK (ready to receive)
    MsgHeader hdr; std::vector<char> payload;
    if (!recvMsg(hdr, payload)) return false;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(payload.data(), payload.size());
        return false;
    }
    if (hdr.cmd != RSP_OK) return false;

    // Open and stream local file with low I/O priority to avoid starving other apps
    HANDLE hFile = CreateFileW(toWide(localPath).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        m_lastError = "Failed to open local file: " + localPath;
        // Send DONE to not leave server hanging
        sendMsg(RSP_DONE);
        return false;
    }
    FILE_IO_PRIORITY_HINT_INFO priorityHint = { (PRIORITY_HINT)1 };
    SetFileInformationByHandle(hFile, FileIoPriorityHintInfo, &priorityHint, sizeof(priorityHint));

    // Triple-pipelined push: send chunk N while reading + CRC-ing chunk N+1
    size_t pushBufSize = sizeof(MsgHeader) + AFM_CHUNK_SIZE;
    std::vector<char> bufA(pushBufSize), bufB(pushBufSize), bufC(pushBufSize);
    char* sendBuf = bufA.data();
    char* readBuf = bufB.data();
    char* spareBuf = bufC.data();
    uint64_t sent = 0;
    bool success = true;
    inlineCrcReset();

    // Pre-read first chunk
    DWORD firstToRead = (DWORD)std::min((uint64_t)AFM_CHUNK_SIZE, fileSize);
    DWORD sendBytes = 0;
    {
        if (!ReadFile(hFile, sendBuf + sizeof(MsgHeader), firstToRead, &sendBytes, nullptr) || sendBytes == 0) {
            CloseHandle(hFile);
            sendMsg(RSP_DONE);
            recvMsg(hdr, payload);
            return false;
        }
        inlineCrcUpdate(sendBuf + sizeof(MsgHeader), sendBytes);
    }

    while (sent < fileSize) {
        MsgHeader* hdrPtr = (MsgHeader*)sendBuf;
        hdrPtr->cmd = RSP_DATA;
        hdrPtr->length = sendBytes;

        uint64_t nextOffset = sent + sendBytes;
        DWORD nextBytes = 0;
        bool hasNext = nextOffset < fileSize;

        // Send current chunk async
        std::future<bool> sendFut = std::async(std::launch::async, [&, sendBuf, sendBytes]() {
            return sendAll(sendBuf, sizeof(MsgHeader) + sendBytes);
        });

        // Read + CRC next chunk while send is in flight
        bool readOk = true;
        if (hasNext) {
            DWORD toRead = (DWORD)std::min((uint64_t)AFM_CHUNK_SIZE, fileSize - nextOffset);
            if (!ReadFile(hFile, readBuf + sizeof(MsgHeader), toRead, &nextBytes, nullptr) || nextBytes == 0) {
                readOk = false;
            }
            if (readOk) inlineCrcUpdate(readBuf + sizeof(MsgHeader), nextBytes);
        }

        bool sendOk = sendFut.get();
        if (!sendOk) { success = false; break; }
        sent += sendBytes;

        if (progress && !progress(sent, fileSize)) {
            success = false;
            m_lastError = "Cancelled";
            break;
        }

        if (!hasNext || !readOk) break;

        // Rotate buffers
        char* oldSendBuf = sendBuf;
        sendBuf = readBuf;
        readBuf = spareBuf;
        spareBuf = oldSendBuf;
        sendBytes = nextBytes;
    }

    CloseHandle(hFile);

    // Send DONE
    sendMsg(RSP_DONE);

    // ALWAYS read server confirmation to keep TCP stream in sync
    // (even on cancel — otherwise the RSP_OK stays in the buffer and corrupts the next PING)
    if (recvMsg(hdr, payload)) {
        if (success && hdr.cmd == RSP_ERROR) {
            m_lastError = std::string(payload.data(), payload.size());
            return false;
        }
    }

    if (success) {
        m_inlineCrc = ~m_inlineCrc;
        m_inlineCrcPath = localPath;
    }

    return success;
}

bool DeviceClient::relayFile(DeviceClient& src, DeviceClient& dst,
                              const std::string& srcPath, const std::string& dstPath,
                              uint64_t fileSize, ProgressCallback progress) {
    if (!src.m_connected) { src.m_lastError = "Source not connected"; return false; }
    if (!dst.m_connected) { dst.m_lastError = "Destination not connected"; return false; }

    LOG_INFO("Transfer", "relayFile START: " + srcPath + " -> " + dstPath);

    // Lock both devices — always lock lower address first to prevent deadlock
    DeviceClient* first = &src < &dst ? &src : &dst;
    DeviceClient* second = &src < &dst ? &dst : &src;
    std::lock_guard<std::mutex> lk1(first->m_mutex);
    std::lock_guard<std::mutex> lk2(second->m_mutex);

    // --- Initiate pull from source device ---
    if (!src.sendMsg(CMD_PULL, srcPath.data(), (uint32_t)srcPath.size())) {
        src.m_lastError = "Failed to send CMD_PULL";
        return false;
    }

    MsgHeader hdr; std::vector<char> payload;
    if (!src.recvMsg(hdr, payload)) { src.m_lastError = "No response to CMD_PULL"; return false; }
    if (hdr.cmd == RSP_ERROR) {
        src.m_lastError = std::string(payload.data(), payload.size());
        return false;
    }
    if (hdr.cmd != RSP_OK || payload.size() < sizeof(PullHeader)) {
        src.m_lastError = "Unexpected response to CMD_PULL";
        return false;
    }

    PullHeader ph;
    memcpy(&ph, payload.data(), sizeof(ph));
    uint64_t actualSize = ph.file_size;
    if (fileSize == 0) fileSize = actualSize;

    // --- Initiate push to destination device ---
    std::vector<char> pushPayload(sizeof(PushHeader) + dstPath.size());
    PushHeader pushHdr = { actualSize };
    memcpy(pushPayload.data(), &pushHdr, sizeof(pushHdr));
    memcpy(pushPayload.data() + sizeof(pushHdr), dstPath.data(), dstPath.size());

    if (!dst.sendMsg(CMD_PUSH, pushPayload.data(), (uint32_t)pushPayload.size())) {
        dst.m_lastError = "Failed to send CMD_PUSH";
        // Drain source pull stream before returning
        while (src.recvAll(&hdr, sizeof(hdr))) {
            if (hdr.cmd == RSP_DONE || hdr.cmd != RSP_DATA || hdr.length == 0) break;
            std::vector<char> drain(hdr.length);
            if (!src.recvAll(drain.data(), hdr.length)) break;
        }
        return false;
    }

    MsgHeader dstHdr; std::vector<char> dstPayload;
    if (!dst.recvMsg(dstHdr, dstPayload) || dstHdr.cmd != RSP_OK) {
        dst.m_lastError = dstHdr.cmd == RSP_ERROR
            ? std::string(dstPayload.data(), dstPayload.size())
            : "Destination rejected CMD_PUSH";
        // Drain source
        while (src.recvAll(&hdr, sizeof(hdr))) {
            if (hdr.cmd == RSP_DONE || hdr.cmd != RSP_DATA || hdr.length == 0) break;
            std::vector<char> drain(hdr.length);
            if (!src.recvAll(drain.data(), hdr.length)) break;
        }
        return false;
    }

    // --- Double-buffered relay loop ---
    // Recv from src into one buffer while async-sending previous buffer to dst
    size_t bufSize = AFM_CHUNK_SIZE + sizeof(MsgHeader);
    std::vector<char> bufA(bufSize), bufB(bufSize);
    char* recvBuf = bufA.data();
    uint64_t relayed = 0;
    bool success = true;
    std::future<bool> sendFut;

    while (true) {
        // Recv next chunk header from source
        if (!src.recvAll(&hdr, sizeof(hdr))) { success = false; src.m_lastError = "recv header failed"; break; }

        if (hdr.cmd == RSP_DONE) break;
        if (hdr.cmd != RSP_DATA || hdr.length == 0) { success = false; src.m_lastError = "unexpected response in relay"; break; }

        // Recv chunk data from source
        if (!src.recvAll(recvBuf + sizeof(MsgHeader), hdr.length)) { success = false; src.m_lastError = "recv data failed"; break; }

        // Wait for previous send to complete before reusing the other buffer
        if (sendFut.valid()) {
            if (!sendFut.get()) { success = false; dst.m_lastError = "send to dst failed"; break; }
        }

        // Prepare MsgHeader for forwarding
        MsgHeader* fwdHdr = (MsgHeader*)recvBuf;
        fwdHdr->cmd = RSP_DATA;
        fwdHdr->length = hdr.length;

        // Async send to destination
        char* sendBuf = recvBuf;
        uint32_t sendLen = sizeof(MsgHeader) + hdr.length;
        sendFut = std::async(std::launch::async, [&dst, sendBuf, sendLen]() {
            return dst.sendAll(sendBuf, sendLen);
        });

        relayed += hdr.length;

        // Progress callback
        if (progress && !progress(relayed, fileSize)) {
            success = false;
            src.m_lastError = "Cancelled";
            break;
        }

        // Swap buffers
        recvBuf = (recvBuf == bufA.data()) ? bufB.data() : bufA.data();
    }

    // Wait for last send
    if (sendFut.valid()) {
        if (!sendFut.get()) { success = false; dst.m_lastError = "send to dst failed"; }
    }

    // Send RSP_DONE to destination
    dst.sendMsg(RSP_DONE);

    // Read destination confirmation
    if (dst.recvMsg(dstHdr, dstPayload)) {
        if (success && dstHdr.cmd == RSP_ERROR) {
            dst.m_lastError = std::string(dstPayload.data(), dstPayload.size());
            success = false;
        }
    }

    // If we broke out early (cancel/error), drain remaining source data
    if (!success && src.m_connected) {
        while (src.recvAll(&hdr, sizeof(hdr))) {
            if (hdr.cmd == RSP_DONE) break;
            if (hdr.cmd != RSP_DATA || hdr.length == 0) break;
            std::vector<char> drain(hdr.length);
            if (!src.recvAll(drain.data(), hdr.length)) break;
        }
    }

    if (success) {
        LOG_INFO("Transfer", "relayFile OK: " + std::to_string(relayed) + " bytes relayed");
    } else {
        LOG_ERROR("Transfer", "relayFile FAILED: src=" + src.m_lastError + " dst=" + dst.m_lastError);
    }
    return success;
}

bool DeviceClient::getDiskSpace(uint64_t& totalBytes, uint64_t& freeBytes) {
    if (!m_connected) { m_lastError = "Not connected"; return false; }
    MsgHeader hdr; std::vector<char> payload;
    if (!runSmallOp(CMD_DISK_SPACE, nullptr, 0, hdr, payload)) return false;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(payload.data(), payload.size());
        return false;
    }
    if (hdr.cmd != RSP_OK || payload.size() < 16) return false;
    memcpy(&totalBytes, payload.data(), 8);
    memcpy(&freeBytes, payload.data() + 8, 8);
    return true;
}

uint64_t DeviceClient::writeRange(const std::string& remotePath, uint64_t offset,
                                   const void* data, uint32_t length) {
    if (!m_connected) { m_lastError = "Not connected"; return 0; }
    std::lock_guard<std::mutex> lk(m_mutex);

    // Build payload: [8B offset][8B length][path]
    uint64_t len64 = length;
    std::vector<char> cmdPayload(16 + remotePath.size());
    memcpy(cmdPayload.data(), &offset, 8);
    memcpy(cmdPayload.data() + 8, &len64, 8);
    memcpy(cmdPayload.data() + 16, remotePath.data(), remotePath.size());

    if (!sendMsg(CMD_WRITE_RANGE, cmdPayload.data(), (uint32_t)cmdPayload.size())) return 0;

    // Wait for RSP_OK (ready)
    MsgHeader hdr; std::vector<char> resp;
    if (!recvMsg(hdr, resp)) return 0;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(resp.data(), resp.size());
        return 0;
    }
    if (hdr.cmd != RSP_OK) return 0;

    // Send data as RSP_DATA
    MsgHeader dataHdr = { RSP_DATA, length };
    if (!sendAll(&dataHdr, sizeof(dataHdr))) return 0;
    if (!sendAll(data, length)) return 0;

    // Send RSP_DONE
    if (!sendMsg(RSP_DONE)) return 0;

    // Receive confirmation with bytes written
    if (!recvMsg(hdr, resp)) return 0;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(resp.data(), resp.size());
        return 0;
    }
    if (hdr.cmd != RSP_OK || resp.size() < 8) return 0;

    uint64_t written;
    memcpy(&written, resp.data(), 8);
    return written;
}

uint64_t DeviceClient::writeRangeStreaming(const std::string& remotePath, uint64_t remoteOffset,
                                           const std::string& localPath, uint64_t localOffset,
                                           uint64_t length, ProgressCallback progress) {
    if (!m_connected) { m_lastError = "Not connected"; return 0; }
    std::lock_guard<std::mutex> lk(m_mutex);

    // Open local file
    HANDLE hFile = CreateFileW(toWide(localPath).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        m_lastError = "Failed to open local file: " + localPath;
        return 0;
    }

    // Seek to localOffset
    LARGE_INTEGER seekPos; seekPos.QuadPart = (LONGLONG)localOffset;
    SetFilePointerEx(hFile, seekPos, nullptr, FILE_BEGIN);

    // Build payload: [8B offset][8B length][path]
    std::vector<char> cmdPayload(16 + remotePath.size());
    memcpy(cmdPayload.data(), &remoteOffset, 8);
    memcpy(cmdPayload.data() + 8, &length, 8);
    memcpy(cmdPayload.data() + 16, remotePath.data(), remotePath.size());

    if (!sendMsg(CMD_WRITE_RANGE, cmdPayload.data(), (uint32_t)cmdPayload.size())) {
        CloseHandle(hFile);
        return 0;
    }

    // Wait for RSP_OK (server opened file, seeked to offset)
    MsgHeader hdr; std::vector<char> resp;
    if (!recvMsg(hdr, resp)) { CloseHandle(hFile); return 0; }
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(resp.data(), resp.size());
        CloseHandle(hFile);
        return 0;
    }
    if (hdr.cmd != RSP_OK) { CloseHandle(hFile); return 0; }

    // Triple-pipelined streaming: send chunk N while reading chunk N+1
    size_t pushBufSize = sizeof(MsgHeader) + AFM_CHUNK_SIZE;
    std::vector<char> bufA(pushBufSize), bufB(pushBufSize), bufC(pushBufSize);
    char* sendBuf = bufA.data();
    char* readBuf = bufB.data();
    char* spareBuf = bufC.data();
    uint64_t sent = 0;
    bool success = true;

    // Pre-read first chunk
    DWORD firstToRead = (DWORD)std::min((uint64_t)AFM_CHUNK_SIZE, length);
    DWORD sendBytes = 0;
    if (!ReadFile(hFile, sendBuf + sizeof(MsgHeader), firstToRead, &sendBytes, nullptr) || sendBytes == 0) {
        CloseHandle(hFile);
        sendMsg(RSP_DONE);
        recvMsg(hdr, resp);
        return 0;
    }

    while (sent < length) {
        MsgHeader* hdrPtr = (MsgHeader*)sendBuf;
        hdrPtr->cmd = RSP_DATA;
        hdrPtr->length = sendBytes;

        uint64_t nextOffset = sent + sendBytes;
        DWORD nextBytes = 0;
        bool hasNext = nextOffset < length;

        // Send current chunk async
        std::future<bool> sendFut = std::async(std::launch::async, [&, sendBuf, sendBytes]() {
            return sendAll(sendBuf, sizeof(MsgHeader) + sendBytes);
        });

        // Read next chunk while send is in flight
        bool readOk = true;
        if (hasNext) {
            DWORD toRead = (DWORD)std::min((uint64_t)AFM_CHUNK_SIZE, length - nextOffset);
            if (!ReadFile(hFile, readBuf + sizeof(MsgHeader), toRead, &nextBytes, nullptr) || nextBytes == 0)
                readOk = false;
        }

        bool sendOk = sendFut.get();
        if (!sendOk) { success = false; break; }
        sent += sendBytes;

        if (progress && !progress(sent, length)) {
            success = false;
            m_lastError = "Cancelled";
            break;
        }

        if (!hasNext || !readOk) break;

        // Rotate buffers
        char* oldSendBuf = sendBuf;
        sendBuf = readBuf;
        readBuf = spareBuf;
        spareBuf = oldSendBuf;
        sendBytes = nextBytes;
    }

    CloseHandle(hFile);

    // Send DONE
    sendMsg(RSP_DONE);

    // Always read server confirmation to keep TCP stream in sync
    if (recvMsg(hdr, resp)) {
        if (success && hdr.cmd == RSP_ERROR) {
            m_lastError = std::string(resp.data(), resp.size());
            return 0;
        }
    }

    return success ? sent : 0;
}

bool DeviceClient::createFile(const std::string& remotePath, uint64_t totalSize) {
    if (!m_connected) { m_lastError = "Not connected"; return false; }
    std::lock_guard<std::mutex> lk(m_mutex);

    std::vector<char> payload(8 + remotePath.size());
    memcpy(payload.data(), &totalSize, 8);
    memcpy(payload.data() + 8, remotePath.data(), remotePath.size());

    if (!sendMsg(CMD_CREATE_FILE, payload.data(), (uint32_t)payload.size())) return false;

    MsgHeader hdr; std::vector<char> resp;
    if (!recvMsg(hdr, resp)) return false;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(resp.data(), resp.size());
        return false;
    }
    return hdr.cmd == RSP_OK;
}

uint64_t DeviceClient::readRange(const std::string& remotePath, uint64_t offset, uint64_t length,
                                 void* outBuffer) {
    if (!m_connected) { m_lastError = "Not connected"; return 0; }
    std::lock_guard<std::mutex> lk(m_mutex);

    // Build payload: [8B offset][8B length][path]
    std::vector<char> payload(16 + remotePath.size());
    memcpy(payload.data(), &offset, 8);
    memcpy(payload.data() + 8, &length, 8);
    memcpy(payload.data() + 16, remotePath.data(), remotePath.size());

    if (!sendMsg(CMD_READ_RANGE, payload.data(), (uint32_t)payload.size())) return 0;

    MsgHeader hdr; std::vector<char> resp;
    if (!recvMsg(hdr, resp)) return 0;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(resp.data(), resp.size());
        return 0;
    }
    if (hdr.cmd != RSP_OK || resp.size() < 8) return 0;

    uint64_t actualLen;
    memcpy(&actualLen, resp.data(), 8);

    // Receive data chunks into outBuffer
    uint64_t received = 0;
    char* dst = (char*)outBuffer;

    while (true) {
        if (!recvAll(&hdr, sizeof(hdr))) break;
        if (hdr.cmd == RSP_DONE) break;
        if (hdr.cmd != RSP_DATA || hdr.length == 0) break;

        if (received + hdr.length <= actualLen) {
            if (!recvAll(dst + received, hdr.length)) break;
        } else {
            // Overflow safety — recv into transfer buf and copy what fits
            if (m_transferBuf.size() < hdr.length) m_transferBuf.resize(hdr.length);
            if (!recvAll(m_transferBuf.data(), hdr.length)) break;
            uint64_t toCopy = actualLen - received;
            if (toCopy > 0) memcpy(dst + received, m_transferBuf.data(), (size_t)toCopy);
        }
        received += hdr.length;
    }

    return std::min(received, actualLen);
}

uint64_t DeviceClient::readRangeStreaming(const std::string& remotePath, uint64_t offset, uint64_t length,
                                           ReadRangeCallback chunkCallback) {
    if (!m_connected) { m_lastError = "Not connected"; return 0; }
    std::lock_guard<std::mutex> lk(m_mutex);

    // Build payload: [8B offset][8B length][path]
    std::vector<char> payload(16 + remotePath.size());
    memcpy(payload.data(), &offset, 8);
    memcpy(payload.data() + 8, &length, 8);
    memcpy(payload.data() + 16, remotePath.data(), remotePath.size());

    if (!sendMsg(CMD_READ_RANGE, payload.data(), (uint32_t)payload.size())) return 0;

    MsgHeader hdr; std::vector<char> resp;
    if (!recvMsg(hdr, resp)) return 0;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(resp.data(), resp.size());
        return 0;
    }
    if (hdr.cmd != RSP_OK || resp.size() < 8) return 0;

    uint64_t actualLen;
    memcpy(&actualLen, resp.data(), 8);

    // Stream chunks to callback
    uint64_t received = 0;
    if (m_transferBuf.size() < AFM_CHUNK_SIZE) m_transferBuf.resize(AFM_CHUNK_SIZE);

    while (true) {
        if (!recvAll(&hdr, sizeof(hdr))) break;
        if (hdr.cmd == RSP_DONE) break;
        if (hdr.cmd != RSP_DATA || hdr.length == 0) break;

        if (m_transferBuf.size() < hdr.length) m_transferBuf.resize(hdr.length);
        if (!recvAll(m_transferBuf.data(), hdr.length)) break;

        if (!chunkCallback(m_transferBuf.data(), hdr.length, offset + received)) {
            // Callback aborted — drain remaining data
            while (recvAll(&hdr, sizeof(hdr))) {
                if (hdr.cmd == RSP_DONE) break;
                if (hdr.cmd != RSP_DATA || hdr.length == 0) break;
                if (m_transferBuf.size() < hdr.length) m_transferBuf.resize(hdr.length);
                if (!recvAll(m_transferBuf.data(), hdr.length)) break;
            }
            break;
        }
        received += hdr.length;
    }

    return std::min(received, actualLen);
}

bool DeviceClient::resumePullFile(const std::string& remotePath, const std::string& localPath,
                                  uint64_t& outFileSize, ProgressCallback progress) {
    // Check how much we already have locally
    uint64_t localSize = 0;
    try {
        if (std::filesystem::exists(toFsPath(localPath)))
            localSize = std::filesystem::file_size(toFsPath(localPath));
    } catch (...) {}

    if (localSize == 0) {
        // No partial file - just do a normal pull
        return pullFile(remotePath, localPath, outFileSize, progress);
    }

    std::lock_guard<std::mutex> lk(m_mutex);

    // Build payload: [8B offset][path]
    std::vector<char> payload(8 + remotePath.size());
    memcpy(payload.data(), &localSize, 8);
    memcpy(payload.data() + 8, remotePath.data(), remotePath.size());

    if (!sendMsg(CMD_RESUME_PULL, payload.data(), (uint32_t)payload.size())) return false;

    MsgHeader hdr; std::vector<char> resp;
    if (!recvMsg(hdr, resp)) return false;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(resp.data(), resp.size());
        return false;
    }
    if (hdr.cmd != RSP_OK || resp.size() < sizeof(PullHeader)) return false;

    PullHeader ph;
    memcpy(&ph, resp.data(), sizeof(ph));
    outFileSize = ph.file_size;

    // Open local file in append mode
    HANDLE hFile = CreateFileW(toWide(localPath).c_str(), GENERIC_WRITE, 0, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        m_lastError = "Failed to open local file: " + localPath;
        return false;
    }

    // Seek to end (append)
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)localSize;
    SetFilePointerEx(hFile, li, nullptr, FILE_BEGIN);

    uint64_t received = localSize;
    bool success = true;

    while (true) {
        if (!recvAll(&hdr, sizeof(hdr))) { success = false; break; }
        if (hdr.cmd == RSP_DONE) break;
        if (hdr.cmd != RSP_DATA || hdr.length == 0) { success = false; break; }

        if (m_transferBuf.size() < hdr.length) m_transferBuf.resize(hdr.length);
        if (!recvAll(m_transferBuf.data(), hdr.length)) { success = false; break; }

        DWORD written;
        WriteFile(hFile, m_transferBuf.data(), hdr.length, &written, nullptr);
        received += written;

        if (progress && !progress(received, outFileSize)) {
            success = false; m_lastError = "Cancelled"; break;
        }
    }

    CloseHandle(hFile);

    // Always drain until RSP_DONE to keep stream clean
    if (m_connected) {
        while (recvAll(&hdr, sizeof(hdr))) {
            if (hdr.cmd == RSP_DONE) break;
            if (hdr.cmd != RSP_DATA || hdr.length == 0) break;
            if (m_transferBuf.size() < hdr.length) m_transferBuf.resize(hdr.length);
            if (!recvAll(m_transferBuf.data(), hdr.length)) break;
        }
    }

    return success;
}

bool DeviceClient::resumePushFile(const std::string& localPath, const std::string& remotePath,
                                  uint64_t fileSize, ProgressCallback progress) {
    std::lock_guard<std::mutex> lk(m_mutex);

    // Build payload: [8B total_size][path]
    std::vector<char> payload(8 + remotePath.size());
    memcpy(payload.data(), &fileSize, 8);
    memcpy(payload.data() + 8, remotePath.data(), remotePath.size());

    if (!sendMsg(CMD_RESUME_PUSH, payload.data(), (uint32_t)payload.size())) return false;

    // Server responds with RSP_OK + [8B offset] = how much it already has
    MsgHeader hdr; std::vector<char> resp;
    if (!recvMsg(hdr, resp)) return false;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(resp.data(), resp.size());
        return false;
    }
    if (hdr.cmd != RSP_OK || resp.size() < 8) return false;

    uint64_t offset = 0;
    memcpy(&offset, resp.data(), 8);

    // Open local file and seek to offset — low I/O priority to avoid starving other apps
    HANDLE hFile = CreateFileW(toWide(localPath).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        m_lastError = "Failed to open local file: " + localPath;
        sendMsg(RSP_DONE);
        return false;
    }
    { FILE_IO_PRIORITY_HINT_INFO ph = { (PRIORITY_HINT)1 }; SetFileInformationByHandle(hFile, FileIoPriorityHintInfo, &ph, sizeof(ph)); }

    if (offset > 0) {
        LARGE_INTEGER li; li.QuadPart = (LONGLONG)offset;
        SetFilePointerEx(hFile, li, nullptr, FILE_BEGIN);
    }

    // Stream from offset — reuse transfer buffer
    size_t pushBufSize = sizeof(MsgHeader) + AFM_CHUNK_SIZE;
    if (m_transferBuf.size() < pushBufSize) m_transferBuf.resize(pushBufSize);
    char* buf = m_transferBuf.data();
    uint64_t sent = offset;
    bool success = true;

    while (sent < fileSize) {
        DWORD toRead = (DWORD)std::min((uint64_t)AFM_CHUNK_SIZE, fileSize - sent);
        DWORD bytesRead;
        if (!ReadFile(hFile, buf + sizeof(MsgHeader), toRead, &bytesRead, nullptr) || bytesRead == 0) break;

        MsgHeader* hdrPtr = (MsgHeader*)buf;
        hdrPtr->cmd = RSP_DATA;
        hdrPtr->length = bytesRead;
        if (!sendAll(buf, sizeof(MsgHeader) + bytesRead)) { success = false; break; }
        sent += bytesRead;


        if (progress && !progress(sent, fileSize)) {
            success = false; m_lastError = "Cancelled"; break;
        }
    }

    CloseHandle(hFile);

    sendMsg(RSP_DONE);

    // ALWAYS read server confirmation to keep TCP stream in sync
    if (recvMsg(hdr, resp)) {
        if (success && hdr.cmd == RSP_ERROR) {
            m_lastError = std::string(resp.data(), resp.size());
            return false;
        }
    }
    return success;
}

// CRC32 — slicing-by-8 for ~4-8x speedup over byte-by-byte
static uint32_t g_crc32Table[8][256];
static bool g_crc32Init = false;

static void initCrc32Table() {
    if (g_crc32Init) return;
    // Build base table (slice 0)
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
        g_crc32Table[0][i] = c;
    }
    // Build extended tables (slices 1-7)
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = g_crc32Table[0][i];
        for (int s = 1; s < 8; s++) {
            c = g_crc32Table[0][c & 0xFF] ^ (c >> 8);
            g_crc32Table[s][i] = c;
        }
    }
    g_crc32Init = true;
}

// Slicing-by-8: processes 8 bytes per iteration
static void crc32UpdateSlice8(uint32_t& crc, const void* data, size_t len) {
    initCrc32Table();
    const uint8_t* p = (const uint8_t*)data;

    // Process 8 bytes at a time
    while (len >= 8) {
        uint32_t a = crc ^ *(const uint32_t*)p;
        uint32_t b = *(const uint32_t*)(p + 4);
        crc = g_crc32Table[7][(a      ) & 0xFF] ^
              g_crc32Table[6][(a >>  8) & 0xFF] ^
              g_crc32Table[5][(a >> 16) & 0xFF] ^
              g_crc32Table[4][(a >> 24)       ] ^
              g_crc32Table[3][(b      ) & 0xFF] ^
              g_crc32Table[2][(b >>  8) & 0xFF] ^
              g_crc32Table[1][(b >> 16) & 0xFF] ^
              g_crc32Table[0][(b >> 24)       ];
        p += 8;
        len -= 8;
    }
    // Remaining bytes
    while (len-- > 0)
        crc = g_crc32Table[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
}

void DeviceClient::inlineCrcUpdate(const void* data, size_t len) {
    crc32UpdateSlice8(m_inlineCrc, data, len);
}

void crc32Update(uint32_t& crc, const void* data, size_t len) {
    crc32UpdateSlice8(crc, data, len);
}

// Combine two CRC32 values: crc1 covers [0, split), crc2 covers [split, split+len2)
// Uses GF(2) matrix exponentiation — O(log n), instant even for 100 GB
static uint32_t gf2MatrixTimes(const uint32_t* mat, uint32_t vec) {
    uint32_t sum = 0;
    for (int i = 0; vec; i++, vec >>= 1)
        if (vec & 1) sum ^= mat[i];
    return sum;
}
static void gf2MatrixSquare(uint32_t* sq, const uint32_t* mat) {
    for (int n = 0; n < 32; n++) sq[n] = gf2MatrixTimes(mat, mat[n]);
}

uint32_t crc32Combine(uint32_t crc1, uint32_t crc2, uint64_t len2) {
    if (len2 == 0) return crc1;
    uint32_t even[32], odd[32];
    odd[0] = 0xEDB88320; // polynomial
    uint32_t row = 1;
    for (int n = 1; n < 32; n++) { odd[n] = row; row <<= 1; }
    gf2MatrixSquare(even, odd);
    gf2MatrixSquare(odd, even);
    do {
        gf2MatrixSquare(even, odd);
        if (len2 & 1) crc1 = gf2MatrixTimes(even, crc1);
        len2 >>= 1;
        if (len2 == 0) break;
        gf2MatrixSquare(odd, even);
        if (len2 & 1) crc1 = gf2MatrixTimes(odd, crc1);
        len2 >>= 1;
    } while (len2 != 0);
    return crc1 ^ crc2;
}

static uint32_t computeLocalCrc32(const std::string& path, std::atomic<float>* progress = nullptr) {
    initCrc32Table();
    HANDLE hFile = CreateFileW(toWide(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return 0;
    // Low I/O priority for CRC — don't starve other apps
    { FILE_IO_PRIORITY_HINT_INFO ph = { (PRIORITY_HINT)1 }; SetFileInformationByHandle(hFile, FileIoPriorityHintInfo, &ph, sizeof(ph)); }

    // Get file size for progress
    LARGE_INTEGER fileSize;
    GetFileSizeEx(hFile, &fileSize);
    uint64_t totalSize = (uint64_t)fileSize.QuadPart;
    uint64_t processed = 0;

    uint32_t crc = ~(uint32_t)0;
    const size_t kBufSize = 4 * 1024 * 1024; // 4MB buffer
    char* buf = new char[kBufSize];
    DWORD bytesRead;
    while (ReadFile(hFile, buf, (DWORD)kBufSize, &bytesRead, nullptr) && bytesRead > 0) {
        crc32UpdateSlice8(crc, buf, bytesRead);
        processed += bytesRead;
        if (progress && totalSize > 0)
            progress->store((float)((double)processed / (double)totalSize));
    }
    if (progress) progress->store(1.0f);
    delete[] buf;
    CloseHandle(hFile);
    return ~crc;
}

bool DeviceClient::verifyFileCrc(const std::string& remotePath, const std::string& localPath, std::string& detail,
                                 std::atomic<float>* crcProgress, std::atomic<int>* crcPhase,
                                 double* remoteMs, double* localMs) {
    if (!m_connected) { detail = "Not connected"; return false; }

    // Get local file size to estimate CRC computation time
    uint64_t localFileSize = 0;
    try { localFileSize = std::filesystem::file_size(toFsPath(localPath)); } catch (...) {}

    // Scale recv timeout: ~200MB/s read speed on device, plus margin
    // Minimum 30s, add 1s per 100MB
    DWORD crcTimeout = 30000 + (DWORD)(localFileSize / (100ULL * 1024 * 1024)) * 1000;
    if (crcTimeout > 300000) crcTimeout = 300000; // cap at 5 minutes
    LOG_INFO("CRC", "verifyFileCrc: " + remotePath + " (size=" + std::to_string(localFileSize) + " timeout=" + std::to_string(crcTimeout) + "ms)");

    if (crcPhase) crcPhase->store(1); // phase 1: server computing
    if (crcProgress) crcProgress->store(0.0f);

    auto remoteStart = std::chrono::steady_clock::now();
    uint32_t remoteCrc = 0;
    {
        std::lock_guard<std::mutex> lk(m_mutex);

        // Set long timeout for CRC computation on large files
        SOCKET sock = (SOCKET)m_socket;
        DWORD origTimeout = 10000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&crcTimeout, sizeof(crcTimeout));

        if (!sendMsg(CMD_CRC32, remotePath.data(), (uint32_t)remotePath.size())) {
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));
            detail = "Failed to request remote CRC";
            LOG_ERROR("CRC", detail);
            return false;
        }
        MsgHeader hdr; std::vector<char> payload;
        if (!recvMsg(hdr, payload)) {
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));
            detail = "Failed to receive remote CRC";
            LOG_ERROR("CRC", detail + " (timeout was " + std::to_string(crcTimeout) + "ms)");
            return false;
        }
        // Restore normal timeout
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));

        if (hdr.cmd == RSP_ERROR) {
            detail = "Server error: " + std::string(payload.data(), payload.size());
            LOG_ERROR("CRC", detail);
            return false;
        }
        if (hdr.cmd != RSP_OK || payload.size() < 4) { detail = "Invalid CRC response"; return false; }
        memcpy(&remoteCrc, payload.data(), 4);
    }

    auto remoteEnd = std::chrono::steady_clock::now();
    double remoteDur = std::chrono::duration<double, std::milli>(remoteEnd - remoteStart).count();
    if (remoteMs) *remoteMs = remoteDur;
    LOG_INFO("CRC", "Remote CRC: " + std::to_string(remoteCrc) + " (" + std::to_string((int)remoteDur) + "ms) - checking local CRC...");

    // Phase 2: local CRC — use inline CRC if available (computed during transfer)
    if (crcPhase) crcPhase->store(2);
    if (crcProgress) crcProgress->store(0.0f);
    auto localStart = std::chrono::steady_clock::now();
    uint32_t localCrc;
    if (m_inlineCrcPath == localPath) {
        // Use CRC computed during transfer — zero extra disk I/O
        localCrc = m_inlineCrc;
        if (crcProgress) crcProgress->store(1.0f);
        LOG_INFO("CRC", "Using inline CRC (no re-read needed)");
    } else {
        // Fallback: compute from disk (resume transfers, etc.)
        LOG_INFO("CRC", "No inline CRC — reading file from disk");
        localCrc = computeLocalCrc32(localPath, crcProgress);
    }

    auto localEnd = std::chrono::steady_clock::now();
    double localDur = std::chrono::duration<double, std::milli>(localEnd - localStart).count();
    if (localMs) *localMs = localDur;

    char buf[128];
    snprintf(buf, sizeof(buf), "Local: %08X  Remote: %08X", localCrc, remoteCrc);
    detail = buf;
    LOG_INFO("CRC", std::string(buf) + (localCrc == remoteCrc ? " MATCH" : " MISMATCH") +
             " (remote=" + std::to_string((int)remoteDur) + "ms local=" + std::to_string((int)localDur) + "ms)");

    return localCrc == remoteCrc;
}

bool DeviceClient::deleteFile(const std::string& path) {
    if (!m_connected) { m_lastError = "Not connected"; return false; }
    MsgHeader hdr; std::vector<char> payload;
    if (!runSmallOp(CMD_DELETE, path.data(), (uint32_t)path.size(), hdr, payload)) return false;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(payload.data(), payload.size());
        return false;
    }
    return hdr.cmd == RSP_OK;
}

bool DeviceClient::createDirectory(const std::string& path) {
    if (!m_connected) { m_lastError = "Not connected"; return false; }
    MsgHeader hdr; std::vector<char> payload;
    if (!runSmallOp(CMD_MKDIR, path.data(), (uint32_t)path.size(), hdr, payload)) return false;
    if (hdr.cmd == RSP_ERROR) { m_lastError = std::string(payload.data(), payload.size()); return false; }
    return hdr.cmd == RSP_OK;
}

bool DeviceClient::renameFile(const std::string& oldPath, const std::string& newPath) {
    if (!m_connected) { m_lastError = "Not connected"; return false; }
    uint32_t oldLen = (uint32_t)oldPath.size();
    std::vector<char> payload(4 + oldPath.size() + newPath.size());
    memcpy(payload.data(), &oldLen, 4);
    memcpy(payload.data() + 4, oldPath.data(), oldPath.size());
    memcpy(payload.data() + 4 + oldPath.size(), newPath.data(), newPath.size());
    MsgHeader hdr; std::vector<char> resp;
    if (!runSmallOp(CMD_RENAME, payload.data(), (uint32_t)payload.size(), hdr, resp)) return false;
    if (hdr.cmd == RSP_ERROR) { m_lastError = std::string(resp.data(), resp.size()); return false; }
    return hdr.cmd == RSP_OK;
}

uint64_t DeviceClient::getFileSize(const std::string& path) {
    if (!m_connected) return 0;
    MsgHeader hdr; std::vector<char> payload;
    if (!runSmallOp(CMD_STAT, path.data(), (uint32_t)path.size(), hdr, payload)) return 0;
    if (hdr.cmd == RSP_OK && payload.size() >= sizeof(StatResponse)) {
        StatResponse sr;
        memcpy(&sr, payload.data(), sizeof(sr));
        return sr.size;
    }
    return 0;
}
