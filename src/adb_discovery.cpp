#include "adb_discovery.h"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <sstream>
#include <tuple>

std::optional<AdbEndpoint> parseAdbEndpoint(std::string_view address) {
    auto colon = address.rfind(':');
    if (colon == std::string_view::npos || colon == 0) return {};
    auto host = address.substr(0, colon);
    auto portText = address.substr(colon + 1);
    unsigned port = 0;
    auto [end, error] = std::from_chars(portText.data(), portText.data() + portText.size(), port);
    if (error != std::errc{} || end != portText.data() + portText.size() || port == 0 || port > 65535)
        return {};
    if (host.front() == '[') {
        if (host.size() < 4 || host.back() != ']') return {};
        auto ipv6 = host.substr(1, host.size() - 2);
        if (std::count(ipv6.begin(), ipv6.end(), ':') < 2 ||
            !std::all_of(ipv6.begin(), ipv6.end(), [](unsigned char c) {
                return std::isxdigit(c) || c == ':' || c == '.' || c == '%';
            })) return {};
    } else {
        int octets = 0;
        size_t start = 0;
        while (start < host.size()) {
            auto dot = host.find('.', start);
            if (dot == std::string_view::npos) dot = host.size();
            auto part = host.substr(start, dot - start);
            unsigned value = 0;
            auto [last, ec] = std::from_chars(part.data(), part.data() + part.size(), value);
            if (part.empty() || part.size() > 3 || ec != std::errc{} ||
                last != part.data() + part.size() || value > 255) return {};
            ++octets;
            start = dot + 1;
        }
        if (octets != 4 || host.back() == '.') return {};
    }
    return AdbEndpoint{std::string(host), static_cast<uint16_t>(port)};
}

std::vector<AdbMdnsService> parseAdbMdnsServices(std::string_view output) {
    std::vector<AdbMdnsService> services;
    std::istringstream stream{std::string(output)};
    std::string line;
    while (std::getline(stream, line)) {
        std::istringstream fields(line);
        AdbMdnsService service;
        if (!(fields >> service.name >> service.type >> service.address)) continue;
        if (service.type.ends_with('.')) service.type.pop_back();
        if (service.type != "_adb-tls-pairing._tcp" && service.type != "_adb-tls-connect._tcp" &&
            service.type != "_adb._tcp") continue;
        if (!parseAdbEndpoint(service.address)) continue;
        services.push_back(std::move(service));
    }
    return services;
}

std::string findAdbConnectAddress(const std::vector<AdbMdnsService>& services,
    std::string_view pairedAddress, std::string_view guid) {
    auto paired = parseAdbEndpoint(pairedAddress);
    if (!paired) return {};
    std::string match;
    for (const auto& service : services) {
        if (service.type != "_adb-tls-connect._tcp") continue;
        auto endpoint = parseAdbEndpoint(service.address);
        if (!endpoint || endpoint->host != paired->host) continue;
        if (!guid.empty()) {
            if (service.name == guid || service.name == "adb-" + std::string(guid)) return service.address;
        } else {
            if (!match.empty() && match != service.address) return {};
            match = service.address;
        }
    }
    return guid.empty() ? match : std::string{};
}

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
