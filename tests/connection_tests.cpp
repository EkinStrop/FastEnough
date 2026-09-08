#include "adb_discovery.h"
#include "process_runner.h"
#include "transfer_coverage.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static void writeHandle(HANDLE handle, const std::string& data) {
    DWORD written = 0;
    require(WriteFile(handle, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
        written == data.size(), "child output failed");
}

static int childMain(const std::wstring& executable, const std::wstring& mode) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE), err = GetStdHandle(STD_ERROR_HANDLE);
    if (mode == L"hang") { Sleep(60000); return 0; }
    if (mode == L"stderr-first") {
        writeHandle(err, std::string(512 * 1024, 'E'));
        writeHandle(out, std::string(512 * 1024, 'O'));
    } else if (mode == L"mixed") {
        for (int i = 0; i < 128; ++i) {
            writeHandle(out, std::string(4096, 'O'));
            writeHandle(err, std::string(4096, 'E'));
        }
    } else if (mode == L"large") {
        const std::string block(1024 * 1024, 'L');
        for (int i = 0; i < 32; ++i) {
            writeHandle(err, "progress\n");
            writeHandle(out, block);
        }
    } else if (mode == L"echo") {
        char buffer[4096];
        DWORD count = 0;
        while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof(buffer), &count, nullptr) && count)
            writeHandle(out, std::string(buffer, count));
    } else if (mode == L"exit") {
        writeHandle(err, "fake adb failure");
        return 17;
    } else if (mode == L"stdout-error") {
        writeHandle(out, "Failure [INSTALL_FAILED_INSUFFICIENT_STORAGE]");
        return 1;
    } else if (mode == L"spawn" || mode == L"daemon") {
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        std::wstring command = L"\"" + executable + L"\" --child hang";
        require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &info) != 0, "could not start fake daemon");
        writeHandle(out, std::to_string(info.dwProcessId));
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
        if (mode == L"spawn") Sleep(60000);
    } else {
        writeHandle(out, "ready");
    }
    return 0;
}

static void discoveryTests() {
    auto parsed = parseAdbDevices("* daemon started successfully *\r\nList of devices attached\r\n"
        "usb123\tunauthorized usb:1-2 transport_id:4\r\n"
        "192.168.1.2:5555 device product:p model:Pixel_9 device:p transport_id:6\n"
        "adb-serial-random._adb-tls-connect._tcp\toffline transport_id:7\n"
        "[fe80::123%4]:33333 device transport_id:8\n"
        "emulator-5554 device model:sdk transport_id:9\n"
        "restricted no permissions (missing rules) usb:2-1\n"
        "garbage\nerror: device not found\n");
    require(parsed.size() == 6, "parser must ignore headers, daemon messages and malformed lines");
    require(parsed[0].state == "unauthorized" && parsed[0].transportId == 4 &&
        parsed[0].usbAddress == "1-2" && parsed[0].transportType == AdbTransportType::Usb,
        "USB transport metadata must survive parsing");
    require(parsed[1].model == "Pixel_9", "model parsing failed");
    require(parsed[2].transportType == AdbTransportType::Network &&
        parsed[3].transportType == AdbTransportType::Network, "mDNS and IPv6 must be wireless");
    require(parsed[4].transportType == AdbTransportType::Emulator, "emulator must not be USB");
    require(parsed[5].state == "no permissions", "permission failures must stay visible");
    require(isWirelessAdbSerial("adb-phone._adb-tls-pairing._tcp.") &&
        isWirelessAdbSerial("phone.local:5555"), "wireless service names must be recognized");
    require(!isWirelessAdbSerial("usb123") && !isWirelessAdbSerial("phone:0") &&
        !isWirelessAdbSerial("phone:65536") && !isWirelessAdbSerial("phone:12x"),
        "invalid wireless endpoints must be rejected");
    auto invalid = parseAdbDevices("serial device transport_id:12x\nserial2 device transport_id:999999999999999999999\n");
    require(invalid.size() == 2 && !invalid[0].transportId && !invalid[1].transportId,
        "invalid transport IDs must not be partially accepted");

    AdbDeviceSnapshot snapshot;
    require(snapshot.update({}), "initial empty snapshot must be delivered");
    require(!snapshot.update({}), "duplicate empty snapshot must be suppressed");
    require(snapshot.update(parsed), "new devices must be delivered");
    std::reverse(parsed.begin(), parsed.end());
    require(!snapshot.update(parsed), "device order alone must not trigger changes");
    parsed[0].state = "device";
    require(snapshot.update(parsed), "authorization changes must trigger updates");
    parsed[0].transportId++;
    require(snapshot.update(parsed), "transport replacements must trigger updates");
    parsed[0].model = "updated model";
    require(snapshot.update(parsed), "model changes must trigger updates");
    parsed[0].usbAddress = "3-1";
    require(snapshot.update(parsed), "USB address changes must trigger updates");
    require(snapshot.update({}) && snapshot.update(parsed), "same serial reconnect must trigger updates");
    snapshot.reset();
    require(snapshot.update(parsed), "resubscribing must reset the snapshot");
    for (const auto* state : {"offline", "unauthorized", "device"}) {
        parsed[0].state = state;
        require(snapshot.update(parsed), "offline and authorization transitions must not be lost");
    }
}

static void transferTests() {
    TransferCoverage whole(100);
    require(!whole.complete() && whole.record(0, 100) && whole.complete() && whole.copiedBytes() == 100,
        "whole file requires successful coverage");
    TransferCoverage split(100);
    require(split.record(50, 50) && !split.complete(), "an out-of-order tail is not a complete file");
    require(!split.record(40, 20) && !split.record(90, 20), "overlaps and out-of-bounds ranges must be rejected");
    require(split.record(0, 50) && split.complete(), "all successful ranges must complete a split file");
    require(split.record(0, 50) && split.copiedBytes() == 100, "a duplicate acknowledgment must not inflate bytes");
    split.fail();
    require(!split.complete() && split.copiedBytes() == 0 && !split.record(0, 50),
        "a failed file must never regain success from an in-flight worker");
    TransferCoverage missingRange(100);
    require(missingRange.record(0, 20) && missingRange.record(40, 60) && !missingRange.complete(),
        "a gap must fail completion independently of CRC verification");
    require(missingRange.copiedBytes() == 0 && whole.copiedBytes() == 100,
        "partial batch results must count only complete files");
    TransferCoverage skipped(100);
    skipped.skip();
    require(skipped.skipped() && !skipped.record(0, 100) && !skipped.complete(), "skipped files are not copied files");
    TransferCoverage empty(0);
    require(!empty.complete() && empty.record(0, 0) && empty.complete(), "empty file creation must be acknowledged");
    TransferCoverage huge(UINT64_MAX);
    require(!huge.record(UINT64_MAX - 1, 4), "range arithmetic must not overflow");
}

static void processTests(const std::wstring& executable) {
    auto optionsFor = [&](const wchar_t* mode) {
        ProcessOptions options;
        options.commandLine = L"\"" + executable + L"\" --child " + mode;
        options.timeoutMs = 5000;
        return options;
    };
    auto result = runChildProcess(optionsFor(L"mixed"));
    require(result.succeeded() && result.standardOutput == std::string(512 * 1024, 'O') &&
        result.standardError == std::string(512 * 1024, 'E'), "both pipes must drain completely");
    result = runChildProcess(optionsFor(L"exit"));
    require(!result.succeeded() && result.exitCode == 17 && result.standardError == "fake adb failure",
        "exit status and stderr must survive failure");
    result = runChildProcess(optionsFor(L"stdout-error"));
    require(!result.succeeded() && result.diagnostics().find("INSTALL_FAILED_INSUFFICIENT_STORAGE") != std::string::npos,
        "ADB failures printed on stdout must remain visible");
    auto limited = optionsFor(L"mixed");
    limited.captureLimit = 37;
    result = runChildProcess(limited);
    require(result.succeeded() && result.outputTruncated && result.standardOutput.size() == 37 &&
        result.standardError.size() == 37, "capture limits must not block a noisy command");

    auto hang = optionsFor(L"hang");
    hang.timeoutMs = 180;
    auto start = std::chrono::steady_clock::now();
    result = runChildProcess(hang);
    require(result.timedOut && !result.succeeded() &&
        std::chrono::steady_clock::now() - start < std::chrono::seconds(3), "silent command must time out promptly");
    hang.timeoutMs = 5000;
    start = std::chrono::steady_clock::now();
    hang.shouldCancel = [&]() { return std::chrono::steady_clock::now() - start > std::chrono::milliseconds(100); };
    result = runChildProcess(hang);
    require(result.cancelled && !result.timedOut, "silent command must be cancellable");
    hang.shouldCancel = []() { return true; };
    result = runChildProcess(hang);
    require(result.cancelled && result.standardOutput.empty(), "pre-cancelled command must not start");
    auto spawn = optionsFor(L"spawn");
    spawn.timeoutMs = 350;
    result = runChildProcess(spawn);
    require(result.timedOut && !result.standardOutput.empty(), "subprocess timeout must be reported");
    HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, std::stoul(result.standardOutput));
    bool stopped = !child || WaitForSingleObject(child, 2000) == WAIT_OBJECT_0;
    if (child) CloseHandle(child);
    require(stopped, "timed out command must terminate its descendants");
    result = runChildProcess(optionsFor(L"daemon"));
    require(result.succeeded() && !result.standardOutput.empty(), "daemon launcher must complete");
    child = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, std::stoul(result.standardOutput));
    bool alive = child && WaitForSingleObject(child, 0) == WAIT_TIMEOUT;
    if (child) { TerminateProcess(child, 0); WaitForSingleObject(child, 2000); CloseHandle(child); }
    require(alive, "successful command must leave its shared daemon alive");

    wchar_t tempPath[32768];
    require(GetTempPathW(32768, tempPath) != 0, "temporary path unavailable");
    auto directory = std::filesystem::path(tempPath) / (L"FastEnough-tests-" + std::to_wstring(GetCurrentProcessId()));
    require(std::filesystem::create_directory(directory), "test directory must be new");
    struct TempCleanup {
        std::filesystem::path path;
        ~TempCleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{directory};
    auto unicodeExecutable = directory / L"r\u00e9sum\u00e9 \u4e2d.exe";
    std::filesystem::copy_file(executable, unicodeExecutable);
    ProcessOptions unicode;
    unicode.commandLine = L"\"" + unicodeExecutable.wstring() + L"\" --child ready";
    result = runChildProcess(unicode);
    require(result.succeeded() && result.standardOutput == "ready", "Unicode executable path must work");

    auto streamed = optionsFor(L"stderr-first");
    streamed.outputPath = (directory / L"output \u4e2d.bin").wstring();
    result = runChildProcess(streamed);
    require(result.succeeded() && result.bytesWritten == 512 * 1024 &&
        result.standardError.size() == 512 * 1024, "file streaming must drain stderr before stdout arrives");
    auto echo = optionsFor(L"echo");
    echo.inputPath = streamed.outputPath;
    result = runChildProcess(echo);
    require(result.succeeded() && result.standardOutput == std::string(512 * 1024, 'O'),
        "file stdin must preserve the complete payload");
    auto cancelledStream = optionsFor(L"hang");
    auto large = optionsFor(L"large");
    large.outputPath = streamed.outputPath;
    result = runChildProcess(large);
    require(result.succeeded() && result.bytesWritten == 32 * 1024 * 1024 &&
        std::filesystem::file_size(large.outputPath) == result.bytesWritten,
        "large streaming output must complete within its deadline without lost bytes");
    cancelledStream.outputPath = streamed.outputPath;
    start = std::chrono::steady_clock::now();
    cancelledStream.progress = [&](uint64_t) {
        return std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100);
    };
    result = runChildProcess(cancelledStream);
    require(result.cancelled, "file progress cancellation must work without output");
    echo.inputPath = (directory / L"missing.bin").wstring();
    result = runChildProcess(echo);
    require(!result.succeeded() && result.systemError != 0, "missing input must report a Windows error");
    unicode.commandLine = L"\"" + (directory / L"missing.exe").wstring() + L"\"";
    result = runChildProcess(unicode);
    require(!result.succeeded() && result.systemError != 0, "launch failure must not look successful");
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 3 && std::wstring(argv[1]) == L"--child") return childMain(argv[0], argv[2]);
        discoveryTests();
        std::cout << "PASS discovery parsing and lifecycle regressions\n";
        transferTests();
        std::cout << "PASS whole, split, partial, skipped and failed transfer accounting\n";
        processTests(std::filesystem::absolute(argv[0]).wstring());
        std::cout << "PASS process, streaming, timeout, cancellation and daemon regressions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
