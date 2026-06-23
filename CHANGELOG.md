# v1.0.16 (unreleased)

### Improved
* Added a compare button that highlights files missing from the other pane.
* Added a size and SHA-256 check for files that appear on both sides.
* Added folder favorites for quick access to pinned locations.
* Large parallel transfers now keep USB and WiFi connections busy more evenly.
* Transfer progress and speed reporting are now more accurate during parallel copies.
* Folder copies now include contents across Windows and Android transfer modes.
* Saved WiFi devices can now be removed from the no-device screen.
* Root mode is now saved per device and shows a clear dialog when root access is missing or denied.
* APK files can now be installed from a confirmation dialog, including Windows double-click support after registering the APK handler.
* Installed Android apps can now be viewed in a panel and uninstalled from the selected device.

### Fixed
* Windows folders now keep loading when one file cannot be accessed by the system.
* Android connection now works on devices that could not start the device helper.
* Android connection failures now show device helper startup errors in the log.
