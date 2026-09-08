#include "process_runner.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <vector>

namespace {
class Handle {
public:
    explicit Handle(HANDLE handle = nullptr) : m_handle(handle) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return m_handle; }
    bool valid() const { return m_handle && m_handle != INVALID_HANDLE_VALUE; }
    void reset(HANDLE handle = nullptr) {
        if (valid()) CloseHandle(m_handle);
        m_handle = handle;
    }
private:
    HANDLE m_handle;
};

struct AttributeList {
    std::vector<unsigned char> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
    ~AttributeList() { if (value) DeleteProcThreadAttributeList(value); }
};
}

std::string ProcessResult::diagnostics() const {
    std::string message = errorMessage;
    if (!standardError.empty()) {
        if (!message.empty()) message += '\n';
        message += standardError;
    } else if (!succeeded() && !standardOutput.empty()) {
        if (!message.empty()) message += '\n';
        message += standardOutput;
    }
    if (message.empty() && exitCode != 0)
        message = "Command failed with exit code " + std::to_string(exitCode) + ".";
    return message;
}

ProcessResult runChildProcess(const ProcessOptions& options) {
    ProcessResult result;
    auto fail = [&](const char* operation) {
        result.systemError = GetLastError();
        result.errorMessage = std::string(operation) + " (Windows error " + std::to_string(result.systemError) + ").";
    };
    if (options.shouldCancel && options.shouldCancel()) {
        result.cancelled = true;
        result.errorMessage = "Operation cancelled.";
        return result;
    }

    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle outRead, outWrite, errRead, errWrite;
    auto makePipe = [&](Handle& reader, Handle& writer) {
        HANDLE read = nullptr, write = nullptr;
        if (!CreatePipe(&read, &write, &security, 1024 * 1024)) return false;
        reader.reset(read);
        writer.reset(write);
        return SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0) != 0;
    };
    if (!makePipe(outRead, outWrite) || !makePipe(errRead, errWrite)) {
        fail("Could not create command output pipes");
        return result;
    }
    Handle input(CreateFileW(options.inputPath.empty() ? L"NUL" : options.inputPath.c_str(),
        GENERIC_READ, FILE_SHARE_READ, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!input.valid()) {
        fail("Could not open command input");
        return result;
    }
    Handle output;
    if (!options.outputPath.empty()) {
        output.reset(CreateFileW(options.outputPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!output.valid()) {
            fail("Could not create output file");
            return result;
        }
    }

    AttributeList attributes;
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    attributes.storage.resize(attributeBytes);
    auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.storage.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &attributeBytes)) {
        fail("Could not initialize command handles");
        return result;
    }
    attributes.value = list;
    HANDLE inherited[] = {input.get(), outWrite.get(), errWrite.get()};
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited, sizeof(inherited), nullptr, nullptr)) {
        fail("Could not restrict command handles");
        return result;
    }

    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job.valid()) {
        fail("Could not create command job");
        return result;
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.StartupInfo.hStdInput = input.get();
    startup.StartupInfo.hStdOutput = outWrite.get();
    startup.StartupInfo.hStdError = errWrite.get();
    startup.lpAttributeList = list;
    PROCESS_INFORMATION processInfo{};
    std::wstring command = options.commandLine;
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
        nullptr, nullptr, &startup.StartupInfo, &processInfo)) {
        fail("Could not start command");
        return result;
    }
    Handle process(processInfo.hProcess), thread(processInfo.hThread);
    if (!AssignProcessToJobObject(job.get(), process.get())) {
        fail("Could not contain command process");
        TerminateProcess(process.get(), 1);
        WaitForSingleObject(process.get(), 5000);
        return result;
    }
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        fail("Could not resume command");
        TerminateJobObject(job.get(), 1);
        WaitForSingleObject(process.get(), 5000);
        return result;
    }
    outWrite.reset();
    errWrite.reset();
    input.reset();
    thread.reset();

    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(options.timeoutMs ? options.timeoutMs : 30000);
    auto nextProgress = Clock::now();
    std::array<char, 64 * 1024> buffer;
    auto drain = [&](Handle& pipe, std::string& captured, bool writeToFile, bool& hadData) {
        if (!pipe.valid()) return true;
        DWORD available = 0;
        if (!PeekNamedPipe(pipe.get(), nullptr, 0, nullptr, &available, nullptr)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) { pipe.reset(); return true; }
            fail("Could not inspect command output");
            return false;
        }
        if (!available) return true;
        DWORD count = 0;
        if (!ReadFile(pipe.get(), buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &count, nullptr)) {
            fail("Could not read command output");
            return false;
        }
        hadData = hadData || count != 0;
        if (writeToFile) {
            DWORD written = 0;
            if (!WriteFile(output.get(), buffer.data(), count, &written, nullptr) || written != count) {
                fail("Could not write output file");
                return false;
            }
            result.bytesWritten += written;
        } else {
            const size_t take = std::min<size_t>(count, options.captureLimit - captured.size());
            captured.append(buffer.data(), take);
            if (take != count) result.outputTruncated = true;
        }
        return true;
    };

    bool aborted = false;
    for (;;) {
        auto now = Clock::now();
        bool cancel = options.shouldCancel && options.shouldCancel();
        if (!cancel && options.progress && now >= nextProgress) {
            cancel = !options.progress(result.bytesWritten);
            nextProgress = now + std::chrono::milliseconds(50);
        }
        if (cancel || now >= deadline) {
            result.cancelled = cancel;
            result.timedOut = !cancel;
            result.errorMessage = cancel ? "Operation cancelled." : "Command timed out.";
            aborted = true;
            break;
        }
        bool hadData = false;
        if (!drain(outRead, result.standardOutput, output.valid(), hadData) ||
            !drain(errRead, result.standardError, false, hadData)) {
            aborted = true;
            break;
        }
        // A successful ADB command may leave the shared daemon running. Do not wait for its handles.
        DWORD wait = WaitForSingleObject(process.get(), hadData ? 0 : 1);
        if (wait == WAIT_OBJECT_0 && !hadData) {
            if (!drain(outRead, result.standardOutput, output.valid(), hadData) ||
                !drain(errRead, result.standardError, false, hadData)) { aborted = true; break; }
            if (!hadData) break;
        }
        if (wait == WAIT_FAILED) { fail("Could not wait for command"); aborted = true; break; }
    }
    if (aborted) {
        TerminateJobObject(job.get(), 1);
        WaitForSingleObject(process.get(), 5000);
    }
    DWORD exitCode = 1;
    if (!GetExitCodeProcess(process.get(), &exitCode)) fail("Could not read command exit code");
    result.exitCode = exitCode;
    if (!aborted && options.progress && !options.progress(result.bytesWritten)) {
        result.cancelled = true;
        result.errorMessage = "Operation cancelled.";
    }
    // Closing this job must leave a normally started shared ADB daemon alive.
    return result;
}
