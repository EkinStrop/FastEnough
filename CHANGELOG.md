# v1.0.14 (unreleased)

### Added
Added Android to Android direct relay with multi channel transfer support.
Added transfer performance logs with total speed, duration, size, channel count, and per channel throughput.

### Improved
Improved multi device handling so each Android panel keeps the correct selected device.
Improved USB and WiFi pipe controls with 1 to 4 pipes for each transport.
Improved multi channel transfers so multiple files can be distributed across available channels.
Improved Android to Android transfer speed by streaming through memory and reducing Android server write overhead.

### Fixed
Fixed saved WiFi reconnects so the Android explorer opens correctly.
Fixed transfers to Android so files are copied to the device selected in the target panel.
Fixed newly connected devices so they appear in Android panel device selectors without manually selecting them first.
Fixed browsing stability when two Android devices are connected.
Fixed multi file Android deletes so the file list refreshes after the full delete operation.
Fixed Android to Android CRC verification so completed relays are checked inline.
Fixed transfer progress jumps and impossible speed values during multi channel transfers.
Fixed blocked local ADB ports so channel setup can continue on another port.
