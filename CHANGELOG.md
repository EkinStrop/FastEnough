# v1.0.16 (unreleased)

### Improved
* Portable releases now include ADB, so device detection works without a separate Android SDK install.
* Added a compare button that highlights files missing from the other pane.
* Added a size and SHA-256 check for files that appear on both sides.
* Added folder favorites for quick access to pinned locations.
* Compare checks now stay responsive in large folders.
* Compare mode no longer keeps the app in high refresh mode after checks finish.

### Fixed
* The app now finds bundled ADB inside the included platform-tools folder.
* Android SHA-256 compare now refreshes the device helper when an older helper is still running.
