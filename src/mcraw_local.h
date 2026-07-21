#pragma once
#include "app.h"
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <nlohmann/json.hpp>

// List the virtual contents of a local .mcraw file as WindowsFileEntry items
std::vector<WindowsFileEntry> listLocalMcraw(const std::string& mcrawPath);

// Extract a single virtual item from a local .mcraw container to a destination file
// virtualName: e.g. "frame_000001.dng", "audio.wav", "metadata.json"
// Returns true on success
using ProgressCallback = std::function<bool(uint64_t transferred, uint64_t total)>;
bool extractLocalMcrawItem(const std::string& mcrawPath, const std::string& virtualName,
                           const std::string& destPath, ProgressCallback progress = nullptr);

// Batch extraction callback: called after each frame completes
// (completedFrames, totalFrames, totalBytesWritten)
using BatchProgressCallback = std::function<void(int completed, int total, uint64_t bytesWritten)>;

// Extract all DNG frames from a local MCRAW to destDir using parallel threads.
// threadCount <= 0 means auto-detect (hardware_concurrency).
// stopFlag can be used to cancel extraction.
// Returns number of frames successfully extracted.
int extractLocalMcrawBatch(const std::string& mcrawPath,
                           const std::string& destDir,
                           int threadCount = 0,
                           std::atomic<bool>* stopFlag = nullptr,
                           BatchProgressCallback progress = nullptr);

// Fast in-memory DNG generation (used by ProjFS path)
// Builds a DNG directly from decompressed pixel data without TinyDNGWriter overhead.
bool generateDngInMemoryFast(const uint8_t* pixelData, size_t pixelSize,
                             const nlohmann::json& frameMeta,
                             const nlohmann::json& containerMeta,
                             std::vector<uint8_t>& outDng);
