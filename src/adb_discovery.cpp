#include "adb_discovery.h"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <sstream>
#include <tuple>

bool isWirelessAdbSerial(std::string_view serial) {
    std::string lower(serial);
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (lower.ends_with('.')) lower.pop_back();
    if (lower.ends_with("._adb-tls-connect._tcp") ||
        lower.ends_with("._adb-tls-pairing._tcp") || lower.ends_with("._adb._tcp"))
        return true;

    auto colon = serial.rfind(':');
    if (colon == std::string_view::npos || colon == 0) return false;
    auto host = serial.substr(0, colon);
    auto portText = serial.substr(colon + 1);
    unsigned port = 0;
    auto [end, error] = std::from_chars(portText.data(), portText.data() + portText.size(), port);
    if (error != std::errc{} || end != portText.data() + portText.size() || port == 0 || port > 65535)
        return false;
    if (host.front() == '[')
        return host.size() > 3 && host.back() == ']' && host.find(':') != std::string_view::npos;
    return std::all_of(host.begin(), host.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '.' || ch == '-' || ch == '_';
    });
}

std::vector<DeviceInfo> parseAdbDevices(std::string_view output) {
    std::vector<DeviceInfo> devices;
    std::istringstream stream{std::string(output)};
    std::string line;
    while (std::getline(stream, line)) {
        std::istringstream fields(line);
        DeviceInfo device;
        if (!(fields >> device.serial >> device.state)) continue;
        if (device.serial == "error:" || device.serial == "adb:" || device.serial == "*") continue;
        if (device.state == "no") {
            std::string permissions;
            fields >> permissions;
            if (permissions != "permissions") continue;
            device.state = "no permissions";
        }
        if (device.state != "device" && device.state != "offline" &&
            device.state != "unauthorized" && device.state != "authorizing" &&
            device.state != "recovery" && device.state != "sideload" &&
            device.state != "bootloader" && device.state != "rescue" &&
            device.state != "connecting" && device.state != "no permissions" &&
            device.state != "host") continue;

        std::string field;
        while (fields >> field) {
            if (field.starts_with("model:")) device.model = field.substr(6);
            else if (field.starts_with("usb:")) device.usbAddress = field.substr(4);
            else if (field.starts_with("transport_id:")) {
                auto value = std::string_view(field).substr(13);
                uint64_t id = 0;
                auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), id);
                if (error == std::errc{} && end == value.data() + value.size()) device.transportId = id;
            }
        }
        if (device.model.empty()) device.model = device.serial;
        if (device.serial.starts_with("emulator-")) device.transportType = AdbTransportType::Emulator;
        else if (isWirelessAdbSerial(device.serial)) device.transportType = AdbTransportType::Network;
        else if (!device.usbAddress.empty()) device.transportType = AdbTransportType::Usb;
        devices.push_back(std::move(device));
    }
    return devices;
}

bool AdbDeviceSnapshot::update(std::vector<DeviceInfo> devices) {
    std::sort(devices.begin(), devices.end(), [](const DeviceInfo& a, const DeviceInfo& b) {
        return std::tie(a.serial, a.state, a.model, a.usbAddress, a.transportId, a.transportType) <
               std::tie(b.serial, b.state, b.model, b.usbAddress, b.transportId, b.transportType);
    });
    if (m_previous && *m_previous == devices) return false;
    m_previous = std::move(devices);
    return true;
}
