#include "app.h"
#include "ui_panels.h"
#include <algorithm>
#include <cctype>
#include <map>

namespace {
ImVec4 rgb(int r, int g, int b) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return value;
}

std::string wifiAddress(const SavedWifiDevice& device) {
    return device.wifiIp + ":" + std::to_string(device.port);
}

std::string savedName(const SavedWifiDevice& device) {
    if (!device.model.empty()) return device.model;
    if (!device.serial.empty() && device.serial != wifiAddress(device)) return device.serial;
    return device.wifiIp;
}




}

void App::connectFromManager(const std::string& serial, bool savedWifi) {
    if (m_connectionPendingSerial == serial) return;
    m_connectionPendingSerial = serial;
    m_connectionFeedback.clear();
    postAsync("Connecting to " + serial + "...", [this, serial, savedWifi]() {
        UiMessage message;
        message.connectionRequestFinished = true;
        message.connectionSerial = serial;
        message.hasStatus = true;
        try {
            std::string connectSerial = serial;
            const std::string identity = deviceIdentity(serial);
            auto readySlot = [&]() {
                for (int slot = 0; slot < deviceSessionCount(); ++slot)
                    if (deviceSession(slot).connected && deviceSession(slot).client.isServerRunning() &&
                        (deviceSession(slot).serial == connectSerial || deviceIdentity(deviceSession(slot).serial) == identity)) return slot;
                return -1;
            };
            bool connected = true;
            message.connectionSlot = readySlot();
            if (savedWifi && message.connectionSlot < 0) {
                for (const auto& saved : m_prefs.savedWifiDevices) {
                    if (saved.serial != identity || !saved.wirelessDebugging || saved.pairingGuid.empty()) continue;
                    auto discovery = m_device.runAdbCommandResult("mdns services", 5000);
                    for (const auto& service : parseAdbMdnsServices(discovery.standardOutput)) {
                        if (service.type == "_adb-tls-connect._tcp" &&
                            (service.name == saved.pairingGuid || service.name == "adb-" + saved.pairingGuid)) {
                            connectSerial = service.address;
                            break;
                        }
                    }
                    break;
                }
                std::string result = m_device.runAdbCommand("connect " + connectSerial, 8000);
                connected = result.find("connected") != std::string::npos && result.find("failed") == std::string::npos;
                if (!connected) message.status = "Connection failed: " + result;
            }
            if (connected) {
                if (message.connectionSlot < 0) connectDeviceBySerialNow(connectSerial);
                message.connectionSlot = readySlot();
                bool ready = message.connectionSlot >= 0;
                message.status = ready ? "Connected to " + serial : "Could not connect to " + serial + ". Check the device and try again.";
                if (savedWifi && ready) {
                    message.connectedWifiAddress = connectSerial;
                    message.connectedWifiName = queryDeviceDisplayName(connectSerial);
                }
            }
        } catch (const std::exception& error) {
            message.status = "Connection failed: " + std::string(error.what());
        } catch (...) {
            message.status = "Connection failed. Please try again.";
        }
        postUiMessage(std::move(message));
    });
}

void App::renderWifiWizard() {
    if (!m_showWifiWizard) {
        if (m_wizardProbeBusy) {
            ++m_wizardProbeGeneration;
            m_wizardProbeBusy = false;
        }
        return;
    }
    ui::PanelStyle theme(m_resolvedLightTheme);
    const float s = theme.s;
    auto* viewport = ImGui::GetMainViewport();
    ImGui::OpenPopup("WiFi Setup Wizard");
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(.5f, .5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(600 * s, viewport->WorkSize.x - 32 * s),
        std::min(650 * s, viewport->WorkSize.y - 40 * s)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * s, 24 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12 * s);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * s, 10 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, theme.surface);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, theme.text);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, theme.muted);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.border);
    if (ImGui::BeginPopupModal("WiFi Setup Wizard", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar)) {
        const int step = m_wizardStep;
        ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        auto close = [&]() {
            ++m_wizardProbeGeneration;
            m_wizardProbeBusy = false;
            m_showWifiWizard = false;
            if (m_wizardSucceeded) m_wifiBannerDismissed = true;
            ImGui::CloseCurrentPopup();
        };
        theme.tile(ui::Icon::Wifi, origin, 46);
        theme.label(ImVec2(origin.x + 64 * s, origin.y - s), "WiFi Setup Wizard", width - 104 * s, theme.text, 24, true);
        theme.label(ImVec2(origin.x + 64 * s, origin.y + 30 * s), "Connect your phone wirelessly", width - 104 * s, theme.muted, 13);
        if (theme.button("##CloseWifiWizard", "", ui::Icon::Close,
            ImVec2(origin.x + width - 32 * s, origin.y), ImVec2(32 * s, 32 * s), theme.muted)) close();
        ui::tooltip(step == 2 ? "Close this window. Setup will continue in the background." : "Close setup wizard");
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) close();

        const char* steps[] = { "Choose device", "Check WiFi", "Connect" };
        float stepWidth = (width - 16 * s) / 3;
        for (int i = 0; i < 3; ++i) {
            bool complete = step > i && (i < 2 || m_wizardSucceeded);
            bool active = std::min(step, 2) == i;
            ImVec2 p(origin.x + i * (stepWidth + 8 * s), origin.y + 70 * s);
            theme.box(p, ImVec2(stepWidth, 40 * s), active ? theme.raised : theme.background,
                theme.background, active ? theme.blue : theme.border, 7);
            auto ink = complete ? theme.green : active ? theme.blue : theme.muted;
            if (complete) ui::icon(ui::Icon::Check, ImVec2(p.x + 10 * s, p.y + 12 * s), 16 * s, theme.color(ink));
            else theme.label(ImVec2(p.x + 13 * s, p.y + 11 * s), std::to_string(i + 1), 18 * s, ink, 13, true);
            theme.label(ImVec2(p.x + 36 * s, p.y + 11 * s), steps[i], stepWidth - 42 * s, active ? theme.text : theme.muted, 13, active);
        }

        std::vector<DeviceInfo> snapshot;
        {
            std::lock_guard<std::mutex> lock(m_deviceMutex);
            snapshot = m_devices;
        }
        std::vector<DeviceInfo> devices;
        std::map<std::string, size_t> identities;
        for (const auto& device : snapshot) {
            if (device.state != "device") continue;
            auto [it, inserted] = identities.emplace(deviceIdentity(device.serial), devices.size());
            if (inserted) devices.push_back(device);
            else if (isWirelessAdbSerial(devices[it->second].serial) && !isWirelessAdbSerial(device.serial)) devices[it->second] = device;
        }
        auto displayName = [&](const std::string& serial) {
            auto cached = m_deviceDisplayNames.find(serial);
            if (cached != m_deviceDisplayNames.end() && !cached->second.empty()) return cached->second;
            for (const auto& saved : m_prefs.savedWifiDevices)
                if ((serial == saved.serial || serial == wifiAddress(saved)) && !saved.model.empty()) return saved.model;
            for (const auto& device : snapshot)
                if (device.serial == serial && !device.model.empty()) return device.model;
            return serial;
        };
        bool selectionAvailable = false;
        for (const auto& device : devices) {
            if (deviceIdentity(device.serial) == deviceIdentity(m_wizardSerial)) {
                selectionAvailable = true;
                if (step == 0) m_wizardSerial = device.serial;
                break;
            }
        }

        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + 132 * s));
        float bodyHeight = std::max(80 * s, ImGui::GetContentRegionAvail().y - 66 * s);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::BeginChild("##WifiWizardBody", ImVec2(0, bodyHeight));
        ImGui::PopStyleVar();
        float bodyWidth = ImGui::GetContentRegionAvail().x;
        auto paragraph = [&](const std::string& text, ImVec4 color) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
            ImGui::TextColored(color, "%s", text.c_str());
            ImGui::PopTextWrapPos();
        };
        auto heading = [&](const char* title, const char* subtitle) {
            ImVec2 p = ImGui::GetCursorScreenPos();
            theme.label(p, title, bodyWidth, theme.text, 18, true);
            ImGui::Dummy(ImVec2(bodyWidth, 20 * s));
            paragraph(subtitle, theme.muted);
            ImGui::Spacing();
        };
        auto deviceCard = [&](const std::string& serial, bool interactive) {
            ImGui::PushID(serial.c_str());
            ImVec2 p = ImGui::GetCursorScreenPos();
            bool selected = serial == m_wizardSerial;
            if (interactive) {
                if (theme.button("##WizardDevice", "", ui::Icon::None, p, ImVec2(bodyWidth, 66 * s), theme.blue)) {
                    m_wizardSerial = serial;
                    selected = selectionAvailable = true;
                }
                ui::tooltip((displayName(serial) + "\n" + serial).c_str());
                if (selected) theme.box(p, ImVec2(bodyWidth, 66 * s), theme.raised, theme.background, theme.blue, 7);
            } else theme.box(p, ImVec2(bodyWidth, 66 * s), theme.raised, theme.background, theme.border, 7);
            theme.tile(ui::Icon::Phone, ImVec2(p.x + 12 * s, p.y + 13 * s), 40);
            theme.label(ImVec2(p.x + 66 * s, p.y + 13 * s), displayName(serial), bodyWidth - 172 * s, theme.text, 15, true);
            theme.label(ImVec2(p.x + 66 * s, p.y + 39 * s), serial, bodyWidth - 172 * s, theme.muted, 13);
            bool wifi = isWirelessAdbSerial(serial);
            auto ink = wifi ? theme.green : theme.blue;
            ImVec2 badge(p.x + bodyWidth - 106 * s, p.y + 20 * s);
            ImVec4 fill = ink;
            fill.w = .12f;
            ImGui::GetWindowDrawList()->AddRectFilled(badge, ImVec2(badge.x + 66 * s, badge.y + 26 * s), theme.color(fill), 6 * s);
            ui::icon(wifi ? ui::Icon::Wifi : ui::Icon::Usb, ImVec2(badge.x + 8 * s, badge.y + 6 * s), 14 * s, theme.color(ink));
            theme.label(ImVec2(badge.x + 29 * s, badge.y + 5 * s), wifi ? "WiFi" : "USB", 32 * s, ink, 12, true);
            if (interactive) {
                ImVec2 center(p.x + bodyWidth - 22 * s, p.y + 33 * s);
                ImGui::GetWindowDrawList()->AddCircle(center, 8 * s, theme.color(selected ? theme.blue : theme.border), 20, s);
                if (selected) ImGui::GetWindowDrawList()->AddCircleFilled(center, 4 * s, theme.color(theme.blue));
            }
            ImGui::SetCursorScreenPos(p);
            ImGui::Dummy(ImVec2(bodyWidth, 66 * s));
            ImGui::PopID();
        };
        auto statusCard = [&](ui::Icon icon, const char* title, const std::string& detail, ImVec4 ink, bool animated) {
            ImVec2 p = ImGui::GetCursorScreenPos();
            float detailWidth = std::max(60 * s, bodyWidth - 80 * s);
            float detailHeight = ImGui::CalcTextSize(detail.c_str(), nullptr, false, detailWidth).y;
            float height = std::max(92 * s, detailHeight + 56 * s);
            theme.box(p, ImVec2(bodyWidth, height), theme.raised, theme.background, theme.border, 9);
            if (animated) {
                float angle = (float)ImGui::GetTime() * 4;
                auto* draw = ImGui::GetWindowDrawList();
                draw->PathArcTo(ImVec2(p.x + 32 * s, p.y + 33 * s), 12 * s, angle, angle + 4.6f, 24);
                draw->PathStroke(theme.color(ink), 0, 2 * s);
            } else ui::icon(icon, ImVec2(p.x + 18 * s, p.y + 19 * s), 28 * s, theme.color(ink));
            theme.label(ImVec2(p.x + 64 * s, p.y + 18 * s), title, detailWidth, ink, 16, true);
            ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + 64 * s, p.y + 46 * s),
                theme.color(theme.muted), detail.c_str(), nullptr, detailWidth);
            ImGui::Dummy(ImVec2(bodyWidth, height));
        };

        if (step == 0) {
            heading("Choose your device", "Select the phone you want to connect over WiFi.");
            if (devices.empty()) {
                statusCard(ui::Icon::Usb, "Connect a phone to get started", "Available devices will appear here automatically.", theme.blue, false);
                paragraph("1. Connect your phone with a USB data cable.", theme.text);
                paragraph("2. Enable USB debugging in Settings > Developer Options.", theme.text);
                paragraph("3. Unlock your phone and tap Allow when prompted.", theme.text);
                paragraph("To enable Developer Options, tap Build number 7 times in Settings > About phone.", theme.muted);
            } else {
                for (const auto& device : devices) deviceCard(device.serial, true);
                ImGui::Spacing();
                paragraph("Keep USB connected until wireless setup is complete.", theme.muted);
            }
        } else if (step == 1) {
            heading("Check your WiFi connection", "Your phone and computer need to be on the same network.");
            deviceCard(m_wizardSerial, false);
            ImGui::Spacing();
            if (m_wizardProbeBusy) statusCard(ui::Icon::Wifi, "Checking your phone...", "Looking for an active WiFi connection.", theme.blue, true);
            else if (!m_wizardWifiIp.empty()) statusCard(ui::Icon::Wifi, "WiFi is ready", "Phone address: " + m_wizardWifiIp, theme.green, false);
            else statusCard(ui::Icon::Info, "WiFi needs attention", m_wizardProbeError.empty() ?
                "Connect your phone to WiFi, then choose Check again." : m_wizardProbeError, theme.gold, false);
            ImGui::Spacing();
            paragraph("The next step enables wireless access and saves this phone for automatic reconnection.", theme.muted);
        } else if (step == 2) {
            heading("Connecting your phone", "Keep your phone connected while setup finishes.");
            deviceCard(m_wizardSerial, false);
            ImGui::Spacing();
            statusCard(ui::Icon::Wifi, "Setting up wireless access", m_wizardStatus, theme.blue, true);
            ImGui::Spacing();
            paragraph("Setup will continue in the background if you close this window.", theme.muted);
        } else {
            heading(m_wizardSucceeded ? "Your wireless connection is ready" : "Setup could not be completed",
                m_wizardSucceeded ? "Your phone is ready for wireless file transfers." : "Check your connection, then try again.");
            deviceCard(m_wizardSerial, false);
            ImGui::Spacing();
            statusCard(m_wizardSucceeded ? ui::Icon::Check : ui::Icon::Info,
                m_wizardSucceeded ? "Connected over WiFi" : "Connection unsuccessful", m_wizardStatus,
                m_wizardSucceeded ? theme.green : theme.red, false);
            ImGui::Spacing();
            paragraph(m_wizardSucceeded ? "Manage this phone in Saved WiFi devices on the Connections page." :
                "Keep the USB cable connected and check that both devices are on the same WiFi network.", theme.muted);
        }
        ImGui::EndChild();

        ImVec2 footer(origin.x, ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - 64 * s);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(footer.x, footer.y - 16 * s), ImVec2(footer.x + width, footer.y - 16 * s), theme.color(theme.border), s);
        bool back = step == 1 || (step == 3 && !m_wizardSucceeded);
        if (theme.button("##WizardBack", back ? "Back" : step == 0 ? "Cancel" : "Close", back ? ui::Icon::Back : ui::Icon::None,
            footer, ImVec2(100 * s, 40 * s), theme.muted)) {
            if (back) { ++m_wizardProbeGeneration; m_wizardProbeBusy = false; m_wizardStep = 0; }
            else close();
        }
        const char* action = step == 0 ? "Next" : step == 1 ? (m_wizardProbeBusy ? "Checking..." : m_wizardWifiIp.empty() ? "Check again" : "Set up WiFi") :
            step == 2 ? "Setting up..." : m_wizardSucceeded ? "Done" : "Try again";
        bool disabled = (step == 0 && !selectionAvailable) || (step == 1 && m_wizardProbeBusy) || step == 2;
        ImGui::BeginDisabled(disabled);
        if (theme.button("##WizardNext", action, step == 0 ? ui::Icon::Forward : step == 3 && m_wizardSucceeded ? ui::Icon::Check : ui::Icon::Wifi,
            ImVec2(footer.x + width - 148 * s, footer.y), ImVec2(148 * s, 40 * s), rgb(255, 255, 255), 1)) {
            if (step == 0 || (step == 3 && !m_wizardSucceeded)) { m_wizardStep = 1; requestWizardWifiProbe(); }
            else if (step == 1) { if (m_wizardWifiIp.empty()) requestWizardWifiProbe(); else startWizardWifiSetup(); }
            else if (step == 3) close();
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(4);
}

void App::renderConnectionManager(FilePanel& panel) {
    ui::PanelStyle theme(m_resolvedLightTheme);
    const float s = theme.s;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.background);
    ImGui::PushStyleColor(ImGuiCol_Text, theme.text);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, theme.muted);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, theme.raised);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.border);
    ImGui::PushStyleColor(ImGuiCol_CheckMark, theme.blue);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, theme.surface);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, theme.raised);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * s, 18 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * s, 10 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7 * s);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, s);
    ImGui::BeginChild("##ConnectionManager", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);

    ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float padding = 18 * s;
    const float innerWidth = std::max(1.0f, width - 2 * padding);
    float y = origin.y;
    theme.tile(ui::Icon::Link, origin, 56);
    theme.label(ImVec2(origin.x + 78 * s, y - 1 * s), "Connections", width - 172 * s, theme.text, 29, true);
    theme.label(ImVec2(origin.x + 78 * s, y + 36 * s), "USB and wireless connections", width - 80 * s, theme.muted, 16);
    if (theme.button("##ConnectionHelp", "Help", ui::Icon::Help,
        ImVec2(origin.x + width - 84 * s, y + 15 * s), ImVec2(84 * s, 36 * s), theme.text))
        ImGui::OpenPopup("##ConnectionHelpPopup");
    if (ImGui::BeginPopup("##ConnectionHelpPopup")) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(390 * s, width - 24 * s));
        ImGui::TextUnformatted("Connect your Android device");
        ImGui::Separator();
        ImGui::TextWrapped("USB: connect a data cable, enable USB debugging in Developer Options, then tap Allow on your phone.");
        ImGui::Spacing();
        ImGui::TextWrapped("WiFi: keep your phone and computer on the same network. Run Setup Wizard with USB connected, or use WiFi ADB Pairing on Android 11 or newer.");
        ImGui::Spacing();
        ImGui::TextWrapped("Select an available device to connect or open its files. Auto reconnects a saved device when the app starts. Delete removes its saved connection.");
        ImGui::Spacing();
        ImGui::TextWrapped("If a device is missing, unlock it, check the cable or WiFi network, and refresh the device list.");
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
    y += 84 * s;

    std::vector<DeviceInfo> devices;
    {
        std::lock_guard<std::mutex> lock(m_deviceMutex);
        devices = m_devices;
    }
    std::vector<DeviceInfo> physicalDevices;
    std::map<std::string, size_t> identities;
    for (const auto& device : devices) {
        std::string identity = deviceIdentity(device.serial);
        auto [it, inserted] = identities.emplace(identity, physicalDevices.size());
        if (inserted) physicalDevices.push_back(device);
        else {
            auto& existing = physicalDevices[it->second];
            if ((existing.state != "device" && device.state == "device") ||
                (existing.state == device.state && isWirelessAdbSerial(existing.serial) && !isWirelessAdbSerial(device.serial)))
                existing = device;
        }
    }
    int available = (int)std::count_if(physicalDevices.begin(), physicalDevices.end(),
        [](const DeviceInfo& device) { return device.state == "device"; });
    std::string countLabel = std::to_string(available) + (available == 1 ? " device available" : " devices available");
    bool busy = !m_connectionPendingSerial.empty() || deviceFor(panel).isConnecting() ||
        m_primaryReconnectActive || m_wifiTransitionActive;
    if (busy || !m_connectionFeedback.empty()) {
        theme.box(ImVec2(origin.x, y), ImVec2(width, 48 * s), theme.raised, theme.surface, theme.border);
        theme.status(ImVec2(origin.x + padding, y + 15 * s), busy ? "Connecting..." : "Connection status", busy, 150 * s);
        std::string detail = busy ? (!m_connectionPendingSerial.empty() ? m_connectionPendingSerial : deviceFor(panel).statusText()) : m_connectionFeedback;
        theme.label(ImVec2(origin.x + 166 * s, y + 15 * s), detail, width - 210 * s, theme.muted, 13);
        if (!busy && theme.button("##DismissConnectionStatus", "", ui::Icon::Close,
            ImVec2(origin.x + width - 35 * s, y + 9 * s), ImVec2(28 * s, 28 * s), theme.muted)) m_connectionFeedback.clear();
        y += 60 * s;
    }

    auto connectedSlot = [&](const std::string& serial) {
        for (int slot = 0; slot < deviceSessionCount(); ++slot) {
            if (deviceSession(slot).connected && deviceSession(slot).client.isServerRunning() &&
                deviceIdentity(deviceSession(slot).serial) == deviceIdentity(serial)) return slot;
        }
        return -1;
    };
    auto openDevice = [&](const DeviceInfo& device) {
        if (device.state != "device") {
            m_connectionFeedback = device.state == "unauthorized" ? "Unlock your phone and tap Allow on the USB debugging prompt." : "This device is offline. Check its cable or WiFi connection.";
            return;
        }
        int slot = connectedSlot(device.serial);
        FilePanel& target = &panel == &m_leftPanel ? m_leftPanel : m_rightPanel;
        if (slot >= 0) openDeviceSession(target, slot);
        else {
            rememberDeviceView(target);
            ++target.listingGeneration;
            target.refreshInProgress = false;
            target.isAndroid = true;
            target.isApps = target.isConnections = false;
            target.androidEntries.clear();
            target.selectedIndices.clear();
            target.currentPath = "/sdcard";
            strcpy_s(target.pathInput, target.currentPath.c_str());
            target.needsRefresh = true;
        }
        target.pendingDeviceSerial = slot < 0 ? device.serial : "";
        target.pendingDeviceName = slot < 0 ? (device.model.empty() ? device.serial : device.model) : "";
        target.deviceOpenError.clear();
        m_showConnectionsWorkspace = m_showAppsWorkspace = m_showBackupManager = false;
        m_lastFocusedPanel = &target;
        if (slot < 0) connectFromManager(device.serial, false);
    };

    bool wideHeader = innerWidth >= 1030 * s;
    bool tinyHeader = innerWidth < 500 * s;
    float availableHeader = (wideHeader ? 76 : tinyHeader ? 158 : 122) * s;
    int columns = std::clamp((int)((innerWidth + 12 * s) / (272 * s)), 1, 4);
    float cardWidth = (innerWidth - (columns - 1) * 12 * s) / columns;
    std::vector<DeviceInfo> visibleDevices;
    std::string query = lower(m_connectionSearch);
    for (const auto& device : physicalDevices) {
        bool wifi = isWirelessAdbSerial(device.serial);
        if ((m_connectionFilter == 1 && wifi) || (m_connectionFilter == 2 && !wifi)) continue;
        if (lower(device.model + " " + device.serial).find(query) != std::string::npos) visibleDevices.push_back(device);
    }
    int rows = std::max(1, ((int)visibleDevices.size() + columns - 1) / columns);
    float availableHeight = availableHeader + rows * 78 * s + (rows - 1) * 12 * s + padding;
    theme.box(ImVec2(origin.x, y), ImVec2(width, availableHeight), theme.surface, theme.background, theme.border, 10);
    theme.heading(ImVec2(origin.x + padding, y + padding), wideHeader ? 440 * s : innerWidth,
        ui::Icon::Phone, "Available devices", "Devices detected on your network or via USB");

    float controlsY = y + (wideHeader ? 20 : 72) * s;
    float controlsX = wideHeader ? origin.x + width - padding - 566 * s : origin.x + padding;
    float controlsWidth = wideHeader ? 566 * s : innerWidth;
    float searchWidth = tinyHeader ? controlsWidth : controlsWidth - 322 * s;
    theme.search("##AvailableDeviceSearch", "Search devices...", m_connectionSearch, sizeof(m_connectionSearch),
        ImVec2(controlsX, controlsY), searchWidth);
    float filterX = tinyHeader ? controlsX : controlsX + searchWidth + 12 * s;
    float filterY = controlsY + (tinyHeader ? 46 * s : 0);
    ImGui::SetCursorScreenPos(ImVec2(filterX, filterY));
    ImGui::SetNextItemWidth(112 * s);
    const char* filters[] = { "All devices", "USB devices", "WiFi devices" };
    if (ImGui::BeginCombo("##ConnectionTransport", filters[m_connectionFilter], ImGuiComboFlags_NoArrowButton)) {
        for (int i = 0; i < 3; ++i)
            if (ImGui::Selectable(filters[i], m_connectionFilter == i)) m_connectionFilter = i;
        ImGui::EndCombo();
    }
    ui::icon(ui::Icon::Down, ImVec2(filterX + 91 * s, filterY + 12 * s), 13 * s, theme.color(theme.muted));
    ImGui::BeginDisabled(m_connectionsRefreshing);
    if (theme.button("##RefreshConnections", "", ui::Icon::Refresh, ImVec2(filterX + 124 * s, filterY),
        ImVec2(36 * s, 36 * s), theme.blue)) {
        m_connectionsRefreshing = true;
        postAsync("Refreshing devices...", [this]() {
            UiMessage message;
            message.connectionRefreshFinished = true;
            try { message.connectionDevices = m_device.getDevices(); }
            catch (const std::exception& error) { message.hasStatus = true; message.status = error.what(); }
            catch (...) { message.hasStatus = true; message.status = "Could not refresh devices. Please try again."; }
            postUiMessage(std::move(message));
        });
    }
    ui::tooltip("Refresh available USB and wireless devices");
    ImGui::EndDisabled();
    theme.status(ImVec2(filterX + 180 * s, filterY + 9 * s), m_connectionsRefreshing ? "Refreshing..." : countLabel,
        available > 0, controlsX + controlsWidth - filterX - 180 * s);

    float cardsY = y + availableHeader;
    if (visibleDevices.empty()) {
        theme.label(ImVec2(origin.x + padding + 12 * s, cardsY + 12 * s),
            physicalDevices.empty() ? "Waiting for a device" : "No matching devices", innerWidth - 24 * s, theme.text, 16, true);
        theme.label(ImVec2(origin.x + padding + 12 * s, cardsY + 40 * s),
            physicalDevices.empty() ? "Connect via USB or use wireless setup below." : "Try another name, address, or connection filter.",
            innerWidth - 24 * s, theme.muted, 13);
    }
    for (int i = 0; i < (int)visibleDevices.size(); ++i) {
        const auto& device = visibleDevices[i];
        ImVec2 p(origin.x + padding + (i % columns) * (cardWidth + 12 * s), cardsY + (i / columns) * 90 * s);
        ImGui::PushID(device.serial.c_str());
        bool pressed = theme.button("##DeviceCard", "", ui::Icon::None, p, ImVec2(cardWidth, 78 * s), theme.blue);
        bool hovered = ImGui::IsItemHovered();
        const bool ready = connectedSlot(device.serial) >= 0;
        const std::string readiness = ready ? "Files ready. Click to open instantly." :
            device.state == "device" ? "Preparing file access in the background..." : device.state;
        ui::tooltip((device.model + "\n" + device.serial + "\n" + readiness).c_str());
        ui::icon(ui::Icon::Phone, ImVec2(p.x + 18 * s, p.y + 21 * s), 34 * s, theme.color(theme.text));
        theme.label(ImVec2(p.x + 70 * s, p.y + 17 * s), device.model.empty() ? device.serial : device.model,
            cardWidth - 185 * s, theme.text, 14, true);
        theme.label(ImVec2(p.x + cardWidth - 105 * s, p.y + 17 * s),
            ready ? "Ready" : device.state == "device" ? "Preparing..." : "", 90 * s,
            ready ? theme.green : theme.gold, 12);
        bool wifi = isWirelessAdbSerial(device.serial);
        const char* badge = wifi ? "WiFi" : "USB";
        ImVec4 badgeColor = wifi ? theme.green : theme.blue;
        if (device.state != "device") { badge = device.state == "unauthorized" ? "Allow USB" : "Offline"; badgeColor = theme.gold; }
        float badgeWidth = device.state == "unauthorized" ? 80 * s : 65 * s;
        ImVec2 badgePos(p.x + cardWidth - badgeWidth - 14 * s, p.y + 42 * s);
        ImVec4 badgeFill = badgeColor;
        badgeFill.w = .13f;
        ImGui::GetWindowDrawList()->AddRectFilled(badgePos, ImVec2(badgePos.x + badgeWidth, badgePos.y + 25 * s), theme.color(badgeFill), 6 * s);
        ui::icon(wifi ? ui::Icon::Wifi : ui::Icon::Usb, ImVec2(badgePos.x + 8 * s, badgePos.y + 5 * s), 15 * s, theme.color(badgeColor));
        theme.label(ImVec2(badgePos.x + 28 * s, badgePos.y + 5 * s), badge, badgeWidth - 30 * s, badgeColor, 12, true);
        theme.label(ImVec2(p.x + 70 * s, p.y + 43 * s), device.serial, badgePos.x - p.x - 78 * s, theme.muted, 12);
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (pressed) openDevice(device);
        ImGui::PopID();
    }
    y += availableHeight + 18 * s;

    bool wideSavedHeader = innerWidth >= 900 * s;
    bool compactRows = innerWidth < 790 * s;
    bool narrowRows = innerWidth < 330 * s;
    float savedHeader = (wideSavedHeader ? 76 : 122) * s;
    float rowHeight = (compactRows ? (narrowRows ? 134 : 100) : 50) * s;
    float rowGap = 8 * s;
    std::vector<int> savedIndices;
    query = lower(m_savedConnectionSearch);
    for (int i = 0; i < (int)m_prefs.savedWifiDevices.size(); ++i) {
        const auto& saved = m_prefs.savedWifiDevices[i];
        if (!saved.wifiIp.empty() && lower(savedName(saved) + " " + wifiAddress(saved) + " " + saved.serial).find(query) != std::string::npos)
            savedIndices.push_back(i);
    }
    std::stable_sort(savedIndices.begin(), savedIndices.end(), [&](int a, int b) {
        const auto& left = m_prefs.savedWifiDevices[a];
        const auto& right = m_prefs.savedWifiDevices[b];
        if (m_savedConnectionSort == 1) return lower(wifiAddress(left)) < lower(wifiAddress(right));
        if (m_savedConnectionSort == 2 && left.autoConnect != right.autoConnect) return left.autoConnect > right.autoConnect;
        return lower(savedName(left)) < lower(savedName(right));
    });
    float savedHeight = savedHeader + std::max(1, (int)savedIndices.size()) * rowHeight +
        std::max(0, (int)savedIndices.size() - 1) * rowGap + padding;
    theme.box(ImVec2(origin.x, y), ImVec2(width, savedHeight), theme.surface, theme.background, theme.border, 10);
    theme.heading(ImVec2(origin.x + padding, y + padding), wideSavedHeader ? innerWidth - 430 * s : innerWidth,
        ui::Icon::Wifi, "Saved WiFi devices", "Manage your saved wireless devices");
    float savedControlsWidth = wideSavedHeader ? 420 * s : innerWidth;
    float savedControlsX = origin.x + width - padding - savedControlsWidth;
    float savedControlsY = y + (wideSavedHeader ? 20 : 72) * s;
    theme.search("##SavedDeviceSearch", "Search saved devices...", m_savedConnectionSearch, sizeof(m_savedConnectionSearch),
        ImVec2(savedControlsX, savedControlsY), savedControlsWidth - 158 * s);
    const char* sorts[] = { "Sort by name", "Sort by address", "Auto first" };
    ImGui::SetCursorScreenPos(ImVec2(origin.x + width - padding - 146 * s, savedControlsY));
    ImGui::SetNextItemWidth(146 * s);
    if (ImGui::BeginCombo("##SavedDeviceSort", sorts[m_savedConnectionSort], ImGuiComboFlags_NoArrowButton)) {
        for (int i = 0; i < 3; ++i)
            if (ImGui::Selectable(sorts[i], m_savedConnectionSort == i)) m_savedConnectionSort = i;
        ImGui::EndCombo();
    }
    ui::icon(ui::Icon::Down, ImVec2(origin.x + width - padding - 23 * s, savedControlsY + 12 * s), 13 * s, theme.color(theme.muted));
    if (savedIndices.empty()) {
        theme.label(ImVec2(origin.x + padding + 12 * s, y + savedHeader + 5 * s),
            m_prefs.savedWifiDevices.empty() ? "No saved WiFi devices yet" : "No matching saved devices", innerWidth - 24 * s, theme.text, 14, true);
        theme.label(ImVec2(origin.x + padding + 12 * s, y + savedHeader + 29 * s),
            m_prefs.savedWifiDevices.empty() ? "Use the setup wizard or WiFi pairing to add a device." : "Try a different name or address.",
            innerWidth - 24 * s, theme.muted, 13);
    }
    int deleteIndex = -1;
    for (int row = 0; row < (int)savedIndices.size(); ++row) {
        int index = savedIndices[row];
        auto& saved = m_prefs.savedWifiDevices[index];
        std::string address = wifiAddress(saved);
        ImVec2 p(origin.x + padding, y + savedHeader + row * (rowHeight + rowGap));
        theme.box(p, ImVec2(innerWidth, rowHeight), theme.raised, theme.surface, theme.border, 9);
        theme.tile(ui::Icon::Phone, ImVec2(p.x + 16 * s, p.y + 10 * s), 32);
        float actionsX = compactRows ? p.x + 16 * s : p.x + innerWidth - 430 * s;
        float actionsY = p.y + (compactRows ? 56 : 9) * s;
        float textWidth = compactRows ? innerWidth - 76 * s : actionsX - p.x - 82 * s;
        float nameWidth = compactRows ? textWidth : textWidth * .53f;
        theme.label(ImVec2(p.x + 76 * s, p.y + (compactRows ? 10 : 17) * s), savedName(saved), nameWidth, theme.text, 14, true);
        theme.label(ImVec2(compactRows ? p.x + 76 * s : p.x + 76 * s + nameWidth + 12 * s,
            p.y + (compactRows ? 31 : 17) * s), address, compactRows ? textWidth : textWidth - nameWidth - 12 * s, theme.muted, 13);
        ImGui::PushID(address.c_str());
        ImGui::SetCursorScreenPos(ImVec2(actionsX, actionsY + 2 * s));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4 * s, 4 * s));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4 * s);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, saved.autoConnect ? rgb(24, 186, 246) : theme.background);
        ImGui::PushStyleColor(ImGuiCol_CheckMark, rgb(255, 255, 255));
        if (ImGui::Checkbox("Auto##Reconnect", &saved.autoConnect)) m_prefs.save();
        ui::tooltip("Automatically reconnect to this device when the app starts");
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
        float buttonWidth = compactRows ? std::max(30 * s, std::min(114 * s, (innerWidth - (narrowRows ? 70 : 146) * s) * .5f)) : 114 * s;
        float connectX = narrowRows ? actionsX : compactRows ? actionsX + 78 * s : actionsX + 126 * s;
        float buttonY = actionsY + (narrowRows ? 34 * s : 0);
        ImGui::BeginDisabled(!m_connectionPendingSerial.empty());
        if (theme.button("##ConnectSavedDevice", m_connectionPendingSerial == address ? "Connecting" : "Connect", ui::Icon::Wifi,
            ImVec2(connectX, buttonY), ImVec2(buttonWidth, 34 * s), rgb(91, 222, 255), 4)) {
            if (connectedSlot(address) >= 0) openDevice(DeviceInfo{address, savedName(saved), "device"});
            else connectFromManager(address, true);
        }
        ImGui::EndDisabled();
        if (theme.button("##DeleteSavedDevice", "Delete", ui::Icon::Trash,
            ImVec2(connectX + buttonWidth + 16 * s, buttonY), ImVec2(buttonWidth - 10 * s, 34 * s), rgb(255, 143, 151), 3)) deleteIndex = index;
        if (theme.button("##SavedDeviceMore", "", ui::Icon::MoreVertical,
            ImVec2(p.x + innerWidth - 36 * s, buttonY), ImVec2(26 * s, 34 * s), theme.muted)) ImGui::OpenPopup("##SavedDeviceMenu");
        ui::tooltip("More device options");
        if (ImGui::BeginPopup("##SavedDeviceMenu")) {
            ImGui::TextUnformatted(savedName(saved).c_str());
            ImGui::TextDisabled("%s", address.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Copy address")) ImGui::SetClipboardText(address.c_str());
            if (!saved.serial.empty() && ImGui::MenuItem("Copy device serial")) ImGui::SetClipboardText(saved.serial.c_str());
            if (ImGui::MenuItem("Auto-connect on launch", nullptr, &saved.autoConnect)) m_prefs.save();
            ImGui::Separator();
            if (ImGui::MenuItem("Remove saved device")) deleteIndex = index;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (deleteIndex >= 0) {
        std::string name = savedName(m_prefs.savedWifiDevices[deleteIndex]);
        m_prefs.savedWifiDevices.erase(m_prefs.savedWifiDevices.begin() + deleteIndex);
        m_prefs.save();
        m_statusMessage = "Removed saved device: " + name;
        m_statusTime = std::chrono::steady_clock::now();
    }
    y += savedHeight + 18 * s;

    bool wideSetup = innerWidth >= 1020 * s;
    bool horizontalSteps = innerWidth >= 720 * s;
    float stepsWidth = wideSetup ? innerWidth - 364 * s : innerWidth;
    float stepWidth = horizontalSteps ? (stepsWidth - 24 * s) / 3 : stepsWidth;
    const char* stepTitles[] = { "Connect via USB", "Enable Developer Options", "Authorize this computer" };
    const char* stepDescriptions[] = {
        "Connect your phone via USB cable and enable USB debugging.",
        "Go to Settings > About phone and tap Build number 7 times.",
        "When prompted on your device, tap Allow to authorize this computer."
    };
    float stepHeight = 0;
    for (auto description : stepDescriptions)
        stepHeight = std::max(stepHeight, ImGui::GetFont()->CalcTextSizeA(13 * s, FLT_MAX,
            std::max(40 * s, stepWidth - 62 * s), description).y + 36 * s);
    stepHeight = std::max(66 * s, stepHeight);
    float stepsHeight = horizontalSteps ? stepHeight : 3 * (stepHeight + 10 * s) - 10 * s;
    float setupHeight = 76 * s + stepsHeight + padding + (wideSetup ? 0 : 142 * s);
    theme.box(ImVec2(origin.x, y), ImVec2(width, setupHeight), theme.surface, theme.background, theme.border, 10);
    theme.heading(ImVec2(origin.x + padding, y + padding), stepsWidth, ui::Icon::Gear,
        "Setup & Instructions", "Follow these steps to connect your devices");
    for (int i = 0; i < 3; ++i) {
        ImVec2 p(origin.x + padding + (horizontalSteps ? i * (stepWidth + 12 * s) : 0),
            y + 76 * s + (horizontalSteps ? 0 : i * (stepHeight + 10 * s)));
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 21 * s, p.y + 21 * s), 21 * s,
            ImGui::GetColorU32(rgb(29, 72, 129)));
        theme.label(ImVec2(p.x + 15 * s, p.y + 7 * s), std::to_string(i + 1), 20 * s, rgb(69, 210, 255), 21, true);
        theme.label(ImVec2(p.x + 64 * s, p.y + 5 * s), stepTitles[i], stepWidth - 64 * s, theme.text, 13, true);
        ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), 13 * s, ImVec2(p.x + 64 * s, p.y + 29 * s),
            theme.color(theme.muted), stepDescriptions[i], nullptr, std::max(40 * s, stepWidth - 64 * s));
    }
    float setupActionsWidth = wideSetup ? 328 * s : std::min(innerWidth, 440 * s);
    float setupActionsX = wideSetup ? origin.x + width - padding - setupActionsWidth : origin.x + (width - setupActionsWidth) * .5f;
    float setupActionsY = wideSetup ? y + 24 * s : y + 76 * s + stepsHeight + 18 * s;
    if (wideSetup) ImGui::GetWindowDrawList()->AddLine(ImVec2(setupActionsX - 28 * s, y + 20 * s),
        ImVec2(setupActionsX - 28 * s, y + setupHeight - 20 * s), theme.color(theme.border), s);
    if (theme.button("##RunConnectionSetup", "Run Setup Wizard", ui::Icon::Wand,
        ImVec2(setupActionsX, setupActionsY), ImVec2(setupActionsWidth, 38 * s), rgb(255, 255, 255), 1)) {
        openWifiWizard();
    }
    if (theme.button("##PairConnectionWifi", "Pair with QR code (Android 11+)", ui::Icon::Link,
        ImVec2(setupActionsX, setupActionsY + 50 * s), ImVec2(setupActionsWidth, 38 * s), rgb(255, 255, 241), 2)) {
        openWifiPairing();
    }
    float statusWidth = ImGui::CalcTextSize(countLabel.c_str()).x + 20 * s;
    theme.status(ImVec2(setupActionsX + std::max(0.0f, (setupActionsWidth - statusWidth) * .5f), setupActionsY + 106 * s),
        countLabel, available > 0, statusWidth + 10 * s);
    y += setupHeight + 4 * s;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
    ImGui::Dummy(ImVec2(width, s));
    ImGui::EndChild();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(8);
}
