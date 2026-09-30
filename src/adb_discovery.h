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

struct AdbEndpoint {
    std::string host;
    uint16_t port = 0;
};

struct AdbMdnsService {
    std::string name;
    std::string type;
    std::string address;
};

std::optional<AdbEndpoint> parseAdbEndpoint(std::string_view address);
std::vector<AdbMdnsService> parseAdbMdnsServices(std::string_view output);
std::string findAdbConnectAddress(const std::vector<AdbMdnsService>& services,
    std::string_view pairedAddress, std::string_view guid);

class AdbDeviceSnapshot {
public:
    // The first update after subscribing is always significant, including an empty list.
    bool update(std::vector<DeviceInfo> devices);
    void reset() { m_previous.reset(); }

private:
    std::optional<std::vector<DeviceInfo>> m_previous;
};
