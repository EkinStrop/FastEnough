#include "app.h"
#include "device_dokan.h"
#include <algorithm>
#include <map>

std::string App::deviceIdentity(const std::string& serial) const {
    {
        std::lock_guard<std::mutex> lock(m_identityMutex);
        auto found = m_deviceIdentities.find(serial);
        if (found != m_deviceIdentities.end()) return found->second;
    }
    for (const auto& saved : m_prefs.savedWifiDevices) {
        if (saved.serial.empty()) continue;
        if (serial == saved.serial || serial == saved.wifiIp + ":" + std::to_string(saved.port))
            return saved.serial;
        if (!saved.pairingGuid.empty() &&
            (serial == saved.pairingGuid + "._adb-tls-connect._tcp" ||
             serial == "adb-" + saved.pairingGuid + "._adb-tls-connect._tcp")) return saved.serial;
    }
    return serial;
}

void App::discoverDeviceIdentities(const std::vector<DeviceInfo>& devices) {
    for (const auto& device : devices) {
        if (m_shutdownPoll) return;
        if (device.state != "device" || !isWirelessAdbSerial(device.serial)) continue;
        {
            std::lock_guard<std::mutex> lock(m_identityMutex);
            if (m_deviceIdentities.count(device.serial)) continue;
        }
        auto result = m_device.runAdbCommandResult("-s " + device.serial + " shell getprop ro.serialno", 2500);
        if (!result.succeeded()) continue;
        std::string identity = result.standardOutput;
        auto end = identity.find_last_not_of(" \t\r\n");
        identity.resize(end == std::string::npos ? 0 : end + 1);
        if (identity.empty() || identity == "unknown" || identity.find_first_of(" \t\r\n") != std::string::npos) continue;
        std::lock_guard<std::mutex> lock(m_identityMutex);
        m_deviceIdentities[device.serial] = identity;
    }
}

void App::cacheDeviceRoot(int slot) {
    auto& session = deviceSession(slot);
    auto entries = session.client.listDirectory(session.storageRoot);
    if (!session.client.isServerRunning()) return;
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        if (a.isDirectory() != b.isDirectory()) return a.isDirectory();
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    std::lock_guard<std::mutex> lock(session.cacheMutex);
    session.rootEntries = std::move(entries);
    session.rootCached = true;
}

bool App::prepareDeviceSession(int slot, const std::string& serial) {
    auto& session = deviceSession(slot);
    if (session.connected && session.client.isServerRunning()) return true;
    if (m_shutdownPoll) return false;
    session.connected = false;
    session.serial = serial;
    session.client.setAdbPath(m_device.getAdbPath());
    session.client.setLocalPort(slot == 0 ? AFM_PORT : 20000 + slot);
    const bool useRoot = m_prefs.rootEnabledForSerial(deviceIdentity(serial));
    bool started = session.client.startServer(serial, true, useRoot);
    if (!started && useRoot && !m_shutdownPoll) started = session.client.startServer(serial, true, false);
    if (!started) {
        session.retryAfter = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        LOG_WARN("Sessions", "Could not prepare " + serial + ": " + session.client.lastError());
        return false;
    }
    session.storageRoot = session.client.detectStoragePath();
    session.volumes = {session.storageRoot};
    cacheDeviceRoot(slot);
    session.connected = session.client.isServerRunning();
    session.retryAfter = {};
    session.lastHealthCheck = std::chrono::steady_clock::now();
    if (slot == 0) {
        m_androidStorageRoot = session.storageRoot;
        m_androidVolumes = session.volumes;
    }
    UiMessage message;
    message.deviceSessionReady = session.connected;
    message.readyDeviceSlot = slot;
    message.saveDeviceSessions = true;
    postUiMessage(std::move(message));
    LOG_INFO("Sessions", "Ready: " + serial + " in session " + std::to_string(slot));
    return session.connected;
}

void App::maintainDeviceSessions(const std::vector<DeviceInfo>& devices) {
    std::map<int, std::string> online;
    for (const auto& device : devices) {
        if (device.state != "device") continue;
        int slot = m_deviceSessions.ensure(deviceIdentity(device.serial));
        if (deviceSession(slot).serial.empty()) deviceSession(slot).serial = device.serial;
        if (!online.count(slot) || !isWirelessAdbSerial(device.serial)) online[slot] = device.serial;
    }
    if (m_tetheringInProgress || m_wifiTransitionActive) return;
    std::set<int> busySessions;
    {
        std::lock_guard<std::mutex> lock(m_batchMutex);
        for (const auto& batch : m_batchQueue) {
            auto state = batch->state.load();
            if (batch->isLocalCopy) continue;
            if (state == BatchState::Running || state == BatchState::Paused ||
                state == BatchState::Verifying || state == BatchState::WaitingConflict) {
                busySessions.insert(batch->isPull ? batch->srcDeviceSlot : batch->dstDeviceSlot);
                if (batch->isCrossDevice) {
                    busySessions.insert(batch->srcDeviceSlot);
                    busySessions.insert(batch->dstDeviceSlot);
                }
            }
        }
    }
    for (int slot = 1; slot < deviceSessionCount() && !m_shutdownPoll; ++slot) {
        auto& session = deviceSession(slot);
        const auto now = std::chrono::steady_clock::now();
        if (busySessions.count(slot) || DeviceMountManager::instance(slot).ioActive) continue;
        if (session.connected && session.client.isServerRunning() &&
            now - session.lastHealthCheck >= std::chrono::seconds(15)) {
            session.lastHealthCheck = now;
            if (!session.client.verifyConnection()) {
                session.connected = false;
                session.client.disconnectTcp();
            }
        }
        if (session.client.isServerRunning() && session.connected) continue;
        session.connected = false;
        auto found = online.find(slot);
        if (found != online.end() && now >= session.retryAfter)
            prepareDeviceSession(slot, found->second);
    }
}

void App::rememberDeviceView(FilePanel& panel) {
    if (!panel.isAndroid || panel.isApps || panel.isConnections || !panel.pendingDeviceSerial.empty() ||
        panel.deviceSlot < 0 || panel.deviceSlot >= deviceSessionCount()) return;
    auto& session = deviceSession(panel.deviceSlot);
    if (!session.connected || panel.currentPath.empty()) return;
    int side = &panel == &m_leftPanel ? 0 : 1;
    session.views[side] = std::make_unique<FilePanel>(panel);
    session.views[side]->refreshInProgress = false;
}

void App::openDeviceSession(FilePanel& panel, int slot) {
    if (slot < 0 || slot >= deviceSessionCount()) return;
    const bool sameDevice = panel.isAndroid && !panel.isApps && !panel.isConnections &&
        panel.pendingDeviceSerial.empty() && panel.deviceSlot == slot;
    if (sameDevice) return;
    rememberDeviceView(panel);
    auto& session = deviceSession(slot);
    int side = &panel == &m_leftPanel ? 0 : 1;
    const auto generation = panel.listingGeneration + 1;
    if (session.views[side]) panel = *session.views[side];
    else {
        panel = FilePanel{};
        panel.currentPath = session.storageRoot.empty() ? "/sdcard" : session.storageRoot;
        std::lock_guard<std::mutex> lock(session.cacheMutex);
        if (session.rootCached) panel.androidEntries = session.rootEntries;
    }
    panel.listingGeneration = generation;
    panel.isAndroid = true;
    panel.isApps = panel.isConnections = false;
    panel.deviceSlot = slot;
    panel.pendingDeviceSerial.clear();
    panel.pendingDeviceName.clear();
    panel.deviceOpenError.clear();
    panel.selectedIndices.clear();
    panel.focusedIndex = -1;
    panel.refreshInProgress = false;
    panel.navigationTransitionPending = false;
    panel.navigationTransitionReady = false;
    panel.needsRefresh = true;
    strcpy_s(panel.pathInput, panel.currentPath.c_str());
    m_compareDirty = true;
}
