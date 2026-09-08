#pragma once
#include <cstdint>
#include <functional>
#include <string>

struct ProcessOptions {
    std::wstring commandLine;
    std::wstring inputPath;
    std::wstring outputPath;
    uint32_t timeoutMs = 30000;
    size_t captureLimit = 16 * 1024 * 1024;
    std::function<bool()> shouldCancel;
    std::function<bool(uint64_t)> progress;
};

struct ProcessResult {
    uint32_t exitCode = 1;
    uint32_t systemError = 0;
    bool timedOut = false;
    bool cancelled = false;
    bool outputTruncated = false;
    std::string standardOutput;
    std::string standardError;
    std::string errorMessage;
    uint64_t bytesWritten = 0;

    bool succeeded() const {
        return exitCode == 0 && !systemError && !timedOut && !cancelled && errorMessage.empty();
    }
    std::string diagnostics() const;
};

ProcessResult runChildProcess(const ProcessOptions& options);
