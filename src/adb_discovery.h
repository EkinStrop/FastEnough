#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class AdbTransportType { Unknown, Usb, Network, Emulator };

struct DeviceInfo {
    std::string serial;
    std::string model;
    std::string state;
    std::string usbAddress;
    uint64_t transportId = 0;
    AdbTransportType transportType = AdbTransportType::Unknown;

    bool operator==(const DeviceInfo&) const = default;
};

bool isWirelessAdbSerial(std::string_view serial);
std::vector<DeviceInfo> parseAdbDevices(std::string_view output);

class AdbDeviceSnapshot {
public:
    // The first update after subscribing is always significant, including an empty list.
    bool update(std::vector<DeviceInfo> devices);
    void reset() { m_previous.reset(); }

private:
    std::optional<std::vector<DeviceInfo>> m_previous;
};
