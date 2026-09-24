# Connection, transfer, and interface implementation plan

## Scope and completion rules

This plan covers every recommendation in the project review. Existing file management, parallel transfers, MCRAW support, APK management, backups, Explorer integration, and themes remain supported throughout the migration.

Each item is complete only when its production integration and listed verification are finished. Hardware validation is recorded separately from automated tests. An implemented component does not make an unfinished integration complete.

Status: `[ ]` pending, `[x]` implemented and verified. Work proceeds in the dependency order below. Changes remain in the working tree until a commit or release is requested.

## 1. Immediate correctness and regression coverage

- [x] **DISC-01. Complete device snapshots.** Extract ADB device parsing and snapshot comparison. Include serial, state, model, transport ID, and USB address. Process unauthorized and offline state changes. Reset tracking history after empty updates and tracking reconnection. Verify connected, empty, same device connected, authorization transitions, list reordering, and tracking restart.
- [ ] **DISC-02. Wireless transport classification.** Recognize IPv4 endpoints, bracketed IPv6 endpoints, and ADB wireless service names. Do not infer a physical USB connection merely from the absence of an IPv4 address. Verify modern wireless names and ordinary USB serials.
- [x] **ADB-01. Bounded subprocess execution.** Introduce a Unicode process runner with distinct stdout, stderr, exit code, timeout, and cancellation results. Drain output while enforcing a deadline. Terminate only the command process and its children on cancellation. Migrate ordinary commands and file-streaming subprocesses. Verify hanging children, large output on both streams, command failure, cancellation, and Unicode paths.
- [ ] **ADB-02. Shared ADB lifecycle.** Default to leaving the shared ADB server running on exit. Preserve explicit user preferences. Make restart a deliberate troubleshooting operation. Close all owned channels and forwards before optional shutdown. Verify exiting does not disrupt another ADB client under default settings.
- [ ] **FS-01. Isolated write state.** Scope Dokan buffers to the mount and open file handle. Preserve buffered bytes on failure and across nonsequential writes. Verify two mounts writing different content to the same Android path, multiple handles, and writes that change offset.
- [ ] **FS-02. Accurate filesystem results.** Propagate remote create, write, flush, resize, rename, and delete failures. Distinguish a disconnected read from EOF. Implement actual truncation and flush behavior or return an explicit unsupported result. Never report a failed write as successful. Verify disk full, permission denied, disconnect during flush, short writes, and EOF.
- [ ] **TR-01. Exact completion accounting.** Track copied, skipped, failed, and pending work independently. Exhausted retries must not count as copied bytes or successful files. Final success must require complete successful coverage even with CRC checking disabled. Verify whole files, split files, partial success, and disabled verification.
- [ ] **TR-02. Immediate retry and cancellation.** Connect Retry Now to an interruptible retry wait. Wake retry, pause, conflict, and shutdown waits on cancellation. Verify that Retry Now shortens the scheduled wait and Stop ends recovery promptly.
- [ ] **UI-01. Asynchronous WiFi probing.** Remove ADB calls from wizard rendering. Probe on entering the step or selecting Check Again, show progress, and apply the result on the UI thread only if the request is still current. Verify that rendering remains responsive during a slow or failed query.
- [ ] **UI-02. Independent pane connection state.** Connection work for one phone must not replace the other phone's file view. Preserve device, path, selection, and navigation history while disconnected. Disable operations that require the missing device and provide a local reconnect action. Verify disconnecting and reconnecting either phone.

## 2. Device identity, sessions, and endpoint ownership

Depends on the snapshot and subprocess foundations in phase 1.

- [ ] **SES-01. Physical device registry.** Introduce a registry keyed by verified physical identity, distinct from transport serials, IP addresses, or UI slot numbers. Store friendly name, short identity suffix, known transport aliases, and cached metadata. Never merge phones by model name.
- [ ] **SES-02. Per-device sessions.** Give each phone one session owning helper lifecycle, root state, endpoints, connection health, retry policy, forward ownership, and control/transfer channels. Make browsing, transfers, backups, and mounts consume that session.
- [ ] **SES-03. Verified channel attachment.** Reuse existing hardware-serial checks consistently while migrating. Require the new helper handshake to prove session and device identity before admitting every additional or recovered channel. Saved addresses are discovery hints only. Reject stale addresses resolving to another phone.
- [ ] **SES-04. Dynamic forwarding.** Request local ports with ADB `forward tcp:0`, capture the returned port, and remove only mappings owned by the session. Keep local forward ports and remote helper ports as separate fields. Remove fixed-port assumptions from primary, secondary, transfer, backup, NIC, and recovery paths.
- [ ] **SES-05. More than two registered devices.** Replace fixed device-slot storage with stable session handles. Keep two visible file panes, each independently selectable. Connect and recover devices independently without promoting a different phone into a pane.
- [ ] **SES-06. Queue identity binding.** Capture source and destination physical identities when a transfer or backup is queued. Revalidate them before execution and on recovery. Apply the same rule to clipboard contents, favorites, mounts, and delayed actions.
- [ ] **SES-07. UI-thread ownership.** Extend the existing UI message queue to discovery, names, folder/app listings, connection status, preferences, and transfer summaries. Background workers return immutable results. Include session identity, request generation, path, and view type in responses; discard stale results.
- [ ] **SES-08. Independent background work.** Use bounded work queues so an unresponsive phone does not block discovery or browsing on other phones. Keep control operations independent from large transfers. Define socket ownership and cancellation before moving channels between transports.
- [ ] **SES-09. Scoped lifecycle.** Separate disconnecting a channel, releasing a session, and shutting down an owned helper. Reconnecting one channel must not kill a helper used by other channels or mounted drives. Root changes must coordinate active work.

Verification: two identical phone models, two phones on both transports, three or more registered phones, a stale WiFi address pointing at another helper, reordered device lists, queued jobs during device changes, and overlapping backup/mount/transfer activity.

## 3. Authenticated helper protocol and secure direct connections

Depends on session identity and endpoint ownership. Keep security changes coordinated between Android and Windows.

- [ ] **SEC-01. Restricted listener exposure.** Bind the helper to loopback for ADB-only sessions. Open a network listener only for explicitly enabled direct transport, with authentication required before any filesystem command.
- [ ] **SEC-02. Encrypted direct transport.** Integrate a maintained TLS implementation compatible with Windows and Android ARM64. Establish trust through an authorized ADB channel, pin the helper/session identity, and authenticate every control and data connection. Do not send a bearer secret over plaintext TCP. Benchmark encrypted transfer throughput.
- [ ] **SEC-03. Session credentials.** Generate credentials with operating-system cryptographic randomness, scope them to the intended helper session, protect local storage, and expire or revoke them on session shutdown. Keep credentials and wireless pairing codes out of logs.
- [ ] **SEC-04. Versioned handshake.** Exchange device identity, helper instance ID, protocol version, build identity, effective privilege, and capability bits. Fail closed on authentication or identity mismatch. Distinguish incompatible helpers from unreachable endpoints and upgrade helpers through ADB when appropriate.
- [ ] **SEC-05. Shared protocol definition.** Replace duplicate client/server protocol declarations with one portable definition and explicit packed layouts. Add compile-time layout checks and protocol compatibility tests.
- [ ] **SEC-06. Framing and error handling.** Read fixed headers completely, bound every length, validate range arithmetic, and return typed errors for transport, authentication, permissions, missing files, and storage exhaustion. Avoid interpreting arbitrary strings as success or retry instructions.
- [ ] **SEC-07. Root containment.** Verify the helper's actual privilege after attachment. Apply root preferences to the physical device, and require authenticated connections equally in root mode. Expose actual root state separately from requested state.

Verification: unauthenticated file commands, wrong device/session credentials, expired credentials, fragmented messages, oversized payloads, unsupported protocol versions, helper restart, root denied, and trusted connection recovery. Rebuild the Android binary and force Windows resource re-embedding for protocol changes.

## 4. Unified USB and wireless behavior

Depends on phases 2 and 3.

- [ ] **CON-01. Explicit transport policy.** Add per-device Auto, USB only, WiFi only, and USB + WiFi modes. Keep allowed transports separate from stream counts and discovery preferences. Enforce policy in browsing, transfer, backup, NIC, and reconnect paths.
- [ ] **CON-02. Accurate transport types.** Represent USB through ADB, USB tethering, WiFi through ADB, and direct WiFi separately. Derive labels and availability from the actual connection rather than the direct-TCP boolean or endpoint string alone.
- [ ] **CON-03. Auto mode.** Make the first trusted usable path available promptly, then evaluate additional allowed paths. Add channels when measured performance benefits, with bounds on stream count and memory. Preserve the existing adaptive scheduler.
- [ ] **CON-04. Combined-mode recovery.** Continue on the surviving transport, requeue unfinished ranges, and restore the missing transport without restarting healthy channels. USB-only and WiFi-only modes must wait for their allowed transport rather than silently switching.
- [ ] **CON-05. Wireless debugging onboarding.** Make Android wireless debugging the primary wireless pairing flow. Retain manual address entry and an explicitly labeled legacy setup option. Separate pairing endpoints from connection endpoints.
- [ ] **CON-06. mDNS discovery.** Discover pairing and connection services, recognize service-name serials, and refresh dynamic addresses and ports. Reconnect known devices using current discoveries, with manual fallback when discovery is unavailable.
- [ ] **CON-07. Direct WiFi bootstrap.** Use authorized USB ADB to launch and authenticate the helper, then add secure direct WiFi without restarting ADB into legacy TCP mode. Use paired wireless ADB to deploy or restart the helper when USB is absent.
- [ ] **CON-08. Disconnect intent.** Explicit Disconnect suppresses automatic reconnection for that device until the user reconnects or changes the saved preference. Distinguish forget pairing, disconnect session, and disable a transport.
- [ ] **CON-09. USB tethering identity.** During USB mode changes, follow only the intended physical phone. Detect supported tethering interfaces and routes, preserve permitted fallback paths, and report whether the requested USB change actually succeeded.
- [ ] **CON-10. Per-device diagnostics.** Expose ADB availability/version, device authorization, helper readiness, endpoint reachability, selected route/NIC, latency, retry deadline, and actual privilege. Keep routine UI status concise and detailed diagnostics expandable.

Verification: each policy with each transport present/absent; USB unplug/replug; WiFi loss/restoration; network changes; changed wireless port; blocked mDNS; host sleep/resume; explicit disconnect; USB tethering with another phone attached; shared ADB server restart.

## 5. Durable and safe transfers

Build on exact accounting, stable device identities, and the shared session layer.

- [ ] **TR-03. Temporary destinations.** Copy to a uniquely named temporary sibling on the destination filesystem. Preserve an existing destination until successful verification and final rename. Apply this to uploads, downloads, and phone-to-phone transfers. Define conflict behavior if the final destination changes during copying.
- [ ] **TR-04. Persistent job journal.** Save job identity, both device identities, source metadata, final/temporary paths, allowed transport policy, and completed ranges atomically. Recover unfinished jobs after application or helper restart.
- [ ] **TR-05. Resume validation.** Verify source size, modification metadata, and appropriate identity/hash information before resuming. Track completed ranges rather than trusting preallocated file length. Reject changed sources and inconsistent journals.
- [ ] **TR-06. Range integrity.** Require successful coverage without gaps or overlaps before completion. Preserve per-range verification and final integrity results across recovery. Never allow failed work to disappear into completion counters.
- [ ] **TR-07. Explicit partial outcomes.** Present completed, completed with errors, skipped, cancelled, and failed results consistently. Report successfully copied bytes and files, retain useful failure details, and allow retrying failed work only.
- [ ] **TR-08. Cancellable file discovery.** Build recursive transfer manifests off the UI thread, surface inaccessible source folders, and allow cancellation before copying begins.
- [ ] **TR-09. Transfer scheduling extraction.** Move manifest creation, job lifecycle, block scheduling, recovery, verification, and finalization out of the UI class. Keep device-session interfaces explicit and retain adaptive work distribution, stream limits, and CPU/I/O bounds.

Verification: disk full on either destination, permission denial, overwrite then cancellation, partial file failure with CRC off, source modified during resume, application restart, disconnected device during final rename, and interrupted phone-to-phone transfer.

## 6. Connection and file-manager interface

Depends on session snapshots and unified connection policies. Visual work must reflect actual backend state.

- [ ] **UI-03. Coherent device selection.** Give each pane one device selector using friendly names plus short identity suffixes. Remove conflicting primary-device selection behavior. Allow selecting connected and remembered devices without changing another pane.
- [ ] **UI-04. Persistent device status.** Show compact, persistent status for each phone. Distinguish active transport, available standby transport, copying, reconnecting, unauthorized, and disconnected. Let users deliberately expand connection details instead of relying on fading overlays.
- [ ] **UI-05. Simplified main controls.** Put the connection policy in the primary device controls. Move stream counts, NIC binding, root, and advanced diagnostics into expandable device settings. Display requested and active stream counts separately.
- [ ] **UI-06. Local recovery presentation.** Complete the preserved offline pane from phase 1 with clear reason, retry progress, and a reconnect action. Keep healthy panes usable while other devices connect. Preserve cached listings as visibly unavailable data, not actionable live files.
- [ ] **UI-07. Docked transfers.** Add a docked transfer queue showing named source and destination, progress, copied bytes, measured speed, and ETA. Keep channel statistics and detailed failures expandable. Support existing hide/show and taskbar integration.
- [ ] **UI-08. Accurate transfer states.** Present Preparing, Copying, Waiting for phone, Checking files, Completed, Completed with errors, Failed, and Cancelled consistently. Retry Now must perform an action and Stop must remain responsive.
- [ ] **UI-09. Resizable panes.** Add a draggable divider with minimum pane widths, keyboard adjustment, and saved proportion. Preserve each pane's navigation when resizing.
- [ ] **UI-10. Responsive controls.** Wrap or collapse toolbars at smaller widths and high DPI. Remove fixed wide-screen requirements for useful connection information. Verify laptop-sized windows and narrow layouts.
- [ ] **UI-11. Consistent visual system.** Centralize spacing, typography, borders, disabled text, status colors, and active-pane emphasis. Use existing dark/light/custom themes consistently in all dialogs, connection views, and transfer panels. Improve metadata contrast without adding visual clutter.
- [ ] **UI-12. Keyboard and focus.** Make device status, selectors, expanders, retry controls, and the divider keyboard accessible. Use visible focus indicators and avoid hover-only essential information. Preserve existing file navigation shortcuts.
- [ ] **UI-13. Filename rendering and DPI.** Add font fallback/coverage for filenames outside the existing Latin, Greek, and Cyrillic ranges. Verify mixed-script names and handle per-monitor DPI changes without clipped controls or blurry text.
- [ ] **UI-14. Measured transport activity.** Show per-device USB and WiFi rates when transferring, and distinguish ready/idle from active. Do not present requested channels or theoretical link speeds as measured throughput.
- [ ] **UI-15. Verified ancillary state.** Show root and keep-awake as enabled only after the device operation succeeds. Scope both to the physical phone and coordinate teardown/reconnection.

Verification: both themes and custom colors, keyboard-only use, 100/125/150/200 percent scaling, moving between monitors, small windows, long/duplicate phone names, mixed-script filenames, connecting one of several phones, and all transfer outcomes.

## 7. Optional integrations and repeatable builds

- [ ] **BUILD-01. Optional filesystem support.** Load Dokan and ProjFS support only when their features are requested. Normal browsing, transfers, APK operations, and backups must start without those optional dependencies. Explain installation requirements at the relevant feature entry point.
- [ ] **BUILD-02. Helper build dependency.** Add a reproducible Android build script using the installed NDK compiler wrapper, CRC architecture option, and required MotionCam sources. Make changes to helper sources/shared protocol trigger rebuilding and Windows resource re-embedding.
- [ ] **BUILD-03. Platform Tools provenance.** Record and validate the Platform Tools version and package checksum in release metadata. Support an explicit tested version/source and fail packaging if required wireless/forwarding capabilities are missing.
- [ ] **BUILD-04. Document actual requirements.** Update runtime instructions, wireless onboarding, stream limits, optional drivers, architecture, and build toolset requirements to match shipped behavior. Refresh screenshots after the UI is verified.
- [ ] **BUILD-05. Build and package lifecycle.** Close the built app before replacing its executable and relaunch after a successful build. Preserve existing signing and portable ZIP contents. Validate the packaged app and embedded helper versions.
- [ ] **BUILD-06. Modular project structure.** Keep process execution, discovery, registry/sessions, protocol, scheduling, filesystem integration, and rendering in separately testable modules. Update Visual Studio project/filter entries as modules move.

## 8. Validation and delivery

- [ ] **QA-01. Native regression runner.** Add fast tests for parsers, snapshots, identity matching, port ownership, policy decisions, range accounting, and buffer behavior. Compile production components in tests rather than duplicating their implementation.
- [x] **QA-02. Fake ADB integration.** Exercise real subprocess code with a controllable child that produces errors, output, hangs, and cancellation scenarios. Exercise discovery with recorded complete device events and wireless service records.
- [ ] **QA-03. Fake helper integration.** Cover handshake rejection, partial network reads/writes, channel failure, reconnection, disk full, finalization, and integrity failures without requiring phones.
- [ ] **QA-04. Hardware matrix.** Record results for USB reconnect of the same phone, authorization changes, two same-model phones, two phones each on USB + WiFi, more than two registered phones, loss of either transport during copying, two mounts writing the same path, changed WiFi addresses/ports, and disk-full behavior.
- [ ] **QA-05. Performance checks.** Compare sustained large-file throughput, small-file responsiveness, encryption overhead, CPU/memory usage, and UI responsiveness against the existing app on the same hardware. Retain measurements and identify regressions before changing defaults.
- [ ] **QA-06. CI builds.** Add Windows client/test builds and Android helper/protocol validation. Keep signing credentials, machine-specific instructions, and local changelog out of CI artifacts and source commits.
- [ ] **QA-07. Release readiness.** Inspect working/staged files and final diffs, run the supported build/test matrix, maintain short local unreleased notes using patch versions, and publish only when a release is requested. Purge local unreleased notes only after a release is actually published.

## Execution record

### First implementation pass, 2026-09-08

The discovery and subprocess foundations are implemented. Phase 1 is still in progress. No later phase is complete.

| Items | Implemented | Remaining verification or integration |
| --- | --- | --- |
| DISC-01 | Shared parser for polling and tracking, full metadata snapshots, authorization and empty-list transitions, reset on subscription, bounded tracking handshake and partial-frame recovery. | Physical unplug/replug and authorization checks remain in QA-04. Automated snapshot cases pass. |
| DISC-02 | IPv4, bracketed IPv6, hostname endpoints and modern wireless ADB service names. Parsed USB, network, emulator and unknown transport categories. | Replace remaining callers that assume every non-wireless serial is a physical USB transport. |
| ADB-01 | Unicode process launch, separate bounded output streams, exit/error details, deadlines, cancellation, restricted handle inheritance, and command descendant cleanup. Ordinary commands, helper launches, and file streams use the runner. APK installs and archive operations have explicit longer deadlines. | Runner regressions pass. Full backup/install cancellation through the future session layer remains part of SES-02 and TR-02. |
| ADB-02 | New preferences leave shared ADB running. Existing preferences are preserved. Both device slots are stopped before optional shared ADB shutdown. | Verify coexistence with another real ADB client. Complete explicit ownership and removal of every auxiliary forward through SES-04 and SES-09. |
| TR-01, TR-06 | Production range coverage tracks whole and split file success. Failed/retried blocks do not become copied work. Skipped files retain their identity. Final parallel transfer results use complete files and actual copied bytes even with CRC disabled. Existing destinations are excluded from failure cleanup. | Fake-helper fault injection and phone tests, consistent accounting in all transfer paths, source-change validation, and temporary destinations. Existing overwrite operations are not yet atomic. |
| TR-02 | Retry Now advances a generation and wakes reconnect delays. Parallel recovery checks shutdown. Sequential recovery waits are interruptible. | End-to-end Stop/Retry checks during ADB commands, socket connection attempts, pause and conflict waits. |
| UI-01 | WiFi probe runs once on entry or Check Again. Results return through the UI queue with serial/generation checks. Closing or backing out cancels stale probes. Errors are distinguished from missing WiFi. | Rendered slow-query and failed-query checks with a phone or UI harness. |
| UI-02 | Pane connection setup and status use that pane's device. Starting a connection for the other slot no longer replaces this pane. | Preserve offline navigation and cached listings, disable unavailable actions, and add local recovery controls. |
| SEC-03 | Pairing codes are omitted from ADB command logs. | Secure credential storage and the remaining logging audit depend on the authenticated session work. |
| QA-01, QA-02, BUILD-06 | Standalone native tests compile production discovery, subprocess, and coverage code. A fake child exercises stream and process failures. Added a PowerShell test command and updated Visual Studio module entries. | Registry, policy, endpoint, buffer, and helper tests as those components are implemented. |

Validation completed:

- Windows client Release/x64 build passed with zero errors. Existing macro redefinition and third-party conversion warnings remain.
- `tests/run-tests.ps1` configured, built, and passed the native regression suite.
- Regression coverage includes metadata and authorization transitions, empty snapshots, reordered lists, wireless names, malformed ADB output, whole/split coverage, gaps/overlaps, skipped and failed work, Unicode paths, stdout/stderr pressure, a 32 MB file stream, launch failures, deadlines, cancellation, descendant termination, and shared daemon survival.
- The rebuilt application launched, its window responded, and its connected phone passed repeated helper health checks. A subsequent shutdown followed the existing saved ADB shutdown preference.
- Android helper sources were unchanged. No Android helper rebuild, physical failover test, Dokan write test, or visual redesign acceptance test is claimed for this pass.

Next implementation sequence: FS-01 and FS-02 with a fake remote writer, complete the cancellation and offline-pane checks, then the physical registry and per-device session foundation. Encryption, connection policies, durable transfers, the full UI work, packaging, and CI retain all requirements listed above.
