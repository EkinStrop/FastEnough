#define _CRT_SECURE_NO_WARNINGS
#define _CRT_NONSTDC_NO_DEPRECATE
#include "mcraw_projfs.h"
#include "mcraw_local.h"
#include "app.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <objbase.h>
#include <projectedfslib.h>

#pragma comment(lib, "ole32.lib")

#include <motioncam/Decoder.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <algorithm>
#include <unordered_map>
#include <atomic>
#include <cstdio>

#pragma comment(lib, "ProjectedFSLib.lib")

struct VirtualEntry {
    std::wstring name;
    uint64_t size = 0;
    bool isDirectory = false;
    int frameIndex = -1;   // -1 for non-frame entries
    enum Kind { METADATA, FRAME, AUDIO } kind;
};

struct MountContext {
    std::string mcrawPath;
    std::unique_ptr<motioncam::Decoder> decoder;
    nlohmann::json containerMeta;
    std::vector<motioncam::Timestamp> frames;

    std::vector<VirtualEntry> entries; // cached listing
    std::mutex cacheMutex;
    std::unordered_map<int, std::vector<uint8_t>> dngCache; // frameIndex -> DNG bytes

    // Audio cache
    std::vector<uint8_t> audioCache;
    bool audioGenerated = false;

    // Metadata cache
    std::string metadataJson;

    PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT virtContext = nullptr;
};

static bool generateDngInMemory(MountContext* ctx, int frameIndex, std::vector<uint8_t>& outDng) {
    try {
        std::vector<uint8_t> pixelData;
        nlohmann::json frameMeta;
        ctx->decoder->loadFrame(ctx->frames[frameIndex], pixelData, frameMeta);

        return generateDngInMemoryFast(pixelData.data(), pixelData.size(),
                                       frameMeta, ctx->containerMeta, outDng);
    } catch (...) {
        return false;
    }
}

static bool generateWavInMemory(MountContext* ctx, std::vector<uint8_t>& outWav) {
    try {
        std::vector<motioncam::AudioChunk> audioChunks;
        ctx->decoder->loadAudio(audioChunks);
        if (audioChunks.empty()) return false;

        int sampleRate = ctx->decoder->audioSampleRateHz();
        int numChannels = ctx->decoder->numAudioChannels();

        size_t totalSamples = 0;
        for (auto& c : audioChunks) totalSamples += c.second.size();

        uint32_t dataSize = (uint32_t)(totalSamples * sizeof(int16_t));
        uint32_t fileSize = 36 + dataSize;

        std::ostringstream oss(std::ios::binary);
        oss.write("RIFF", 4);
        oss.write((const char*)&fileSize, 4);
        oss.write("WAVE", 4);
        oss.write("fmt ", 4);
        uint32_t fmtSize = 16; oss.write((const char*)&fmtSize, 4);
        uint16_t audioFormat = 1; oss.write((const char*)&audioFormat, 2);
        uint16_t channels = (uint16_t)numChannels; oss.write((const char*)&channels, 2);
        uint32_t sr = (uint32_t)sampleRate; oss.write((const char*)&sr, 4);
        uint32_t byteRate = sr * channels * sizeof(int16_t); oss.write((const char*)&byteRate, 4);
        uint16_t blockAlign = channels * sizeof(int16_t); oss.write((const char*)&blockAlign, 2);
        uint16_t bitsPerSample = 16; oss.write((const char*)&bitsPerSample, 2);
        oss.write("data", 4);
        oss.write((const char*)&dataSize, 4);
        for (auto& c : audioChunks)
            oss.write((const char*)c.second.data(), c.second.size() * sizeof(int16_t));

        std::string s = oss.str();
        outWav.assign(s.begin(), s.end());
        return true;
    } catch (...) {
        return false;
    }
}

static const VirtualEntry* findEntry(MountContext* ctx, const std::wstring& relativePath) {
    // Strip leading backslash
    std::wstring name = relativePath;
    if (!name.empty() && name[0] == L'\\') name = name.substr(1);
    // Also strip trailing backslash
    if (!name.empty() && name.back() == L'\\') name.pop_back();

    for (auto& e : ctx->entries) {
        if (_wcsicmp(e.name.c_str(), name.c_str()) == 0) return &e;
    }
    return nullptr;
}

// Get or generate file data for a virtual entry, returns pointer to cached data
static const std::vector<uint8_t>* getFileData(MountContext* ctx, const VirtualEntry* entry) {
    if (entry->kind == VirtualEntry::METADATA) {
        // Already cached as string during init
        // Return as byte vector — use a static trick with the string
        static thread_local std::vector<uint8_t> metaBuf;
        metaBuf.assign(ctx->metadataJson.begin(), ctx->metadataJson.end());
        return &metaBuf;
    }

    if (entry->kind == VirtualEntry::AUDIO) {
        std::lock_guard<std::mutex> lk(ctx->cacheMutex);
        if (!ctx->audioGenerated) {
            if (!generateWavInMemory(ctx, ctx->audioCache)) return nullptr;
            ctx->audioGenerated = true;
        }
        return &ctx->audioCache;
    }

    if (entry->kind == VirtualEntry::FRAME) {
        std::lock_guard<std::mutex> lk(ctx->cacheMutex);
        auto it = ctx->dngCache.find(entry->frameIndex);
        if (it != ctx->dngCache.end()) return &it->second;

        // Generate DNG
        std::vector<uint8_t> dng;
        if (!generateDngInMemory(ctx, entry->frameIndex, dng)) return nullptr;

        // Cache it (evict old entries if cache gets too large — keep last 8 frames)
        if (ctx->dngCache.size() > 8) {
            // Find the entry with the lowest frame index distance from current
            int curIdx = entry->frameIndex;
            int worstKey = -1;
            int worstDist = 0;
            for (auto& [k, v] : ctx->dngCache) {
                int dist = std::abs(k - curIdx);
                if (dist > worstDist) { worstDist = dist; worstKey = k; }
            }
            if (worstKey >= 0) ctx->dngCache.erase(worstKey);
        }

        auto [ins, _] = ctx->dngCache.emplace(entry->frameIndex, std::move(dng));
        return &ins->second;
    }

    return nullptr;
}

// Directory enumeration state
struct EnumSession {
    std::vector<const VirtualEntry*> sorted;
    size_t index = 0;
    bool filled = false;
};

static std::mutex g_enumMutex;
static std::map<GUID, std::unique_ptr<EnumSession>,
    decltype([](const GUID& a, const GUID& b) {
        return memcmp(&a, &b, sizeof(GUID)) < 0;
    })> g_enumSessions;

static HRESULT CALLBACK cb_StartDirEnum(
    const PRJ_CALLBACK_DATA* cbd,
    const GUID* enumId)
{
    auto* ctx = (MountContext*)cbd->InstanceContext;
    auto session = std::make_unique<EnumSession>();

    // Root listing: add all entries
    for (auto& e : ctx->entries)
        session->sorted.push_back(&e);

    // Sort by ProjFS file name comparison
    std::sort(session->sorted.begin(), session->sorted.end(),
        [](const VirtualEntry* a, const VirtualEntry* b) {
            return PrjFileNameCompare(a->name.c_str(), b->name.c_str()) < 0;
        });

    std::lock_guard<std::mutex> lk(g_enumMutex);
    g_enumSessions[*enumId] = std::move(session);
    return S_OK;
}

static HRESULT CALLBACK cb_EndDirEnum(
    const PRJ_CALLBACK_DATA* cbd,
    const GUID* enumId)
{
    std::lock_guard<std::mutex> lk(g_enumMutex);
    g_enumSessions.erase(*enumId);
    return S_OK;
}

static HRESULT CALLBACK cb_GetDirEnum(
    const PRJ_CALLBACK_DATA* cbd,
    const GUID* enumId,
    PCWSTR searchExpression,
    PRJ_DIR_ENTRY_BUFFER_HANDLE dirEntryBufferHandle)
{
    std::lock_guard<std::mutex> lk(g_enumMutex);
    auto it = g_enumSessions.find(*enumId);
    if (it == g_enumSessions.end()) return E_INVALIDARG;

    auto* session = it->second.get();

    // On restart, reset index
    if (cbd->Flags & PRJ_CB_DATA_FLAG_ENUM_RESTART_SCAN) {
        session->index = 0;
    }

    while (session->index < session->sorted.size()) {
        const VirtualEntry* entry = session->sorted[session->index];

        // Apply search expression filter
        if (searchExpression && !PrjFileNameMatch(entry->name.c_str(), searchExpression)) {
            session->index++;
            continue;
        }

        PRJ_FILE_BASIC_INFO info = {};
        info.IsDirectory = entry->isDirectory ? TRUE : FALSE;
        info.FileSize = entry->size;

        HRESULT hr = PrjFillDirEntryBuffer(entry->name.c_str(), &info, dirEntryBufferHandle);
        if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) {
            // Buffer full, ProjFS will call us again
            return S_OK;
        }
        if (FAILED(hr)) return hr;

        session->index++;
    }

    return S_OK;
}

static HRESULT CALLBACK cb_GetPlaceholderInfo(
    const PRJ_CALLBACK_DATA* cbd)
{
    auto* ctx = (MountContext*)cbd->InstanceContext;
    const VirtualEntry* entry = findEntry(ctx, cbd->FilePathName);
    if (!entry) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

    uint64_t fileSize = entry->size;

    // For audio, generate the WAV on first placeholder request to get exact size
    if (entry->kind == VirtualEntry::AUDIO) {
        const auto* data = getFileData(ctx, entry);
        if (data) fileSize = data->size();
    }

    PRJ_PLACEHOLDER_INFO placeholderInfo = {};
    placeholderInfo.FileBasicInfo.IsDirectory = entry->isDirectory ? TRUE : FALSE;
    placeholderInfo.FileBasicInfo.FileSize = fileSize;

    return PrjWritePlaceholderInfo(
        ctx->virtContext,
        cbd->FilePathName,
        &placeholderInfo,
        sizeof(placeholderInfo));
}

static HRESULT CALLBACK cb_GetFileData(
    const PRJ_CALLBACK_DATA* cbd,
    UINT64 byteOffset,
    UINT32 length)
{
    auto* ctx = (MountContext*)cbd->InstanceContext;
    const VirtualEntry* entry = findEntry(ctx, cbd->FilePathName);
    if (!entry) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

    const auto* data = getFileData(ctx, entry);
    if (!data) return HRESULT_FROM_WIN32(ERROR_READ_FAULT);

    // Clamp to actual data size
    if (byteOffset >= data->size()) return S_OK;
    uint64_t available = data->size() - byteOffset;
    uint32_t toWrite = (uint32_t)std::min((uint64_t)length, available);

    // ProjFS requires aligned buffer allocated with PrjAllocateAlignedBuffer
    void* alignedBuf = PrjAllocateAlignedBuffer(ctx->virtContext, toWrite);
    if (!alignedBuf) return E_OUTOFMEMORY;

    memcpy(alignedBuf, data->data() + byteOffset, toWrite);

    HRESULT hr = PrjWriteFileData(
        ctx->virtContext,
        &cbd->DataStreamId,
        alignedBuf,
        byteOffset,
        toWrite);

    PrjFreeAlignedBuffer(alignedBuf);
    return hr;
}

McrawMount::~McrawMount() {
    unmount();
}

void McrawMount::unmount() {
    if (m_mounted && m_virtContext) {
        PrjStopVirtualizing((PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT)m_virtContext);
        m_virtContext = nullptr;
        m_mounted = false;

        // Delete the MountContext (closes the decoder's FILE* handle on the MCRAW file)
        if (m_mountContext) {
            delete (MountContext*)m_mountContext;
            m_mountContext = nullptr;
        }

        LOG_INFO("MCRAW", "Unmounted ProjFS: " + m_mountPath);
    }

    // Clean up mount directory (best effort)
    if (!m_mountPath.empty()) {
        try { std::filesystem::remove_all(m_mountPath); } catch (...) {}
    }
}

std::shared_ptr<McrawMount> McrawMount::mount(const std::string& mcrawPath, std::string& outMountPath) {
    // Create mount context with decoder
    auto ctx = new MountContext();
    ctx->mcrawPath = mcrawPath;

    try {
        ctx->decoder = std::make_unique<motioncam::Decoder>(mcrawPath);
        ctx->containerMeta = ctx->decoder->getContainerMetadata();
        ctx->frames = ctx->decoder->getFrames(); // copy
        ctx->metadataJson = ctx->containerMeta.dump(2);
    } catch (const std::exception& e) {
        LOG_ERROR("MCRAW", "Failed to open MCRAW: " + std::string(e.what()));
        delete ctx;
        return nullptr;
    }

    // Build virtual entry list
    // metadata.json
    {
        VirtualEntry e;
        e.name = L"metadata.json";
        e.size = ctx->metadataJson.size();
        e.kind = VirtualEntry::METADATA;
        ctx->entries.push_back(std::move(e));
    }

    // Get exact DNG size by generating the first frame (all frames are same resolution)
    uint64_t dngExactSize = 0;
    if (!ctx->frames.empty()) {
        try {
            std::vector<uint8_t> firstDng;
            if (generateDngInMemory(ctx, 0, firstDng)) {
                dngExactSize = firstDng.size();
                // Cache it so we don't regenerate on first read
                std::lock_guard<std::mutex> lk(ctx->cacheMutex);
                ctx->dngCache[0] = std::move(firstDng);
            }
        } catch (...) {}
    }

    // Frame entries
    for (size_t i = 0; i < ctx->frames.size(); i++) {
        VirtualEntry e;
        wchar_t fname[32];
        swprintf(fname, 32, L"frame_%06zu.dng", i + 1);
        e.name = fname;
        e.size = dngExactSize;
        e.kind = VirtualEntry::FRAME;
        e.frameIndex = (int)i;
        ctx->entries.push_back(std::move(e));
    }

    // Audio entry — estimate size from metadata without loading all chunks
    try {
        if (ctx->containerMeta.contains("extraData") &&
            ctx->containerMeta["extraData"].contains("audioSampleRate") &&
            ctx->containerMeta["extraData"].contains("audioChannels")) {
            int sr = ctx->containerMeta["extraData"]["audioSampleRate"];
            int ch = ctx->containerMeta["extraData"]["audioChannels"];
            if (sr > 0 && ch > 0) {
                // Rough estimate: assume audio duration ~ video duration
                // frames / 30fps * sampleRate * channels * 2 bytes
                double durationSec = ctx->frames.size() / 30.0;
                uint64_t estAudioSize = 44 + (uint64_t)(durationSec * sr * ch * sizeof(int16_t));
                VirtualEntry e;
                e.name = L"audio.wav";
                e.size = estAudioSize; // exact size resolved on first read
                e.kind = VirtualEntry::AUDIO;
                ctx->entries.push_back(std::move(e));
            }
        }
    } catch (...) {}

    // Create mount directory
    char tmpDir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpDir);

    // Use hash of mcraw path for unique mount dir
    size_t hash = std::hash<std::string>{}(mcrawPath);
    std::string mountDir = std::string(tmpDir) + "FastEnough_mcraw_" + std::to_string(hash);

    // Clean up any previous mount at this path
    try { std::filesystem::remove_all(mountDir); } catch (...) {}
    try { std::filesystem::create_directories(mountDir); } catch (...) {
        LOG_ERROR("MCRAW", "Failed to create mount dir: " + mountDir);
        delete ctx;
        return nullptr;
    }

    std::wstring mountDirW(mountDir.begin(), mountDir.end());

    // Mark directory as ProjFS virtualization root
    GUID instanceId;
    CoCreateGuid(&instanceId);
    HRESULT hr = PrjMarkDirectoryAsPlaceholder(
        mountDirW.c_str(), nullptr, nullptr, &instanceId);
    if (FAILED(hr)) {
        LOG_ERROR("MCRAW", "PrjMarkDirectoryAsPlaceholder failed: " + std::to_string(hr));
        delete ctx;
        return nullptr;
    }

    // Set up callback table
    PRJ_CALLBACKS callbacks = {};
    callbacks.StartDirectoryEnumerationCallback = cb_StartDirEnum;
    callbacks.EndDirectoryEnumerationCallback = cb_EndDirEnum;
    callbacks.GetDirectoryEnumerationCallback = cb_GetDirEnum;
    callbacks.GetPlaceholderInfoCallback = cb_GetPlaceholderInfo;
    callbacks.GetFileDataCallback = cb_GetFileData;

    // Start virtualizing
    PRJ_STARTVIRTUALIZING_OPTIONS options = {};
    hr = PrjStartVirtualizing(
        mountDirW.c_str(),
        &callbacks,
        ctx,  // InstanceContext — passed to all callbacks
        &options,
        &ctx->virtContext);

    if (FAILED(hr)) {
        LOG_ERROR("MCRAW", "PrjStartVirtualizing failed: " + std::to_string(hr));
        try { std::filesystem::remove_all(mountDir); } catch (...) {}
        delete ctx;
        return nullptr;
    }

    // Create McrawMount object
    auto mount = std::shared_ptr<McrawMount>(new McrawMount());
    mount->m_mcrawPath = mcrawPath;
    mount->m_mountPath = mountDir;
    mount->m_mountPathW = mountDirW;
    mount->m_virtContext = ctx->virtContext;
    mount->m_mountContext = ctx; // store so we can delete it on unmount
    mount->m_mounted = true;

    outMountPath = mountDir;
    LOG_INFO("MCRAW", "ProjFS mounted: " + mcrawPath + " -> " + mountDir);

    return mount;
}

McrawMountManager& McrawMountManager::instance() {
    static McrawMountManager s;
    return s;
}

std::string McrawMountManager::mountMcraw(const std::string& mcrawPath) {
    std::lock_guard<std::mutex> lk(m_mutex);

    // Check if already mounted
    for (auto& m : m_mounts) {
        if (m->mcrawPath() == mcrawPath) return m->mountPath();
    }

    std::string mountPath;
    auto mount = McrawMount::mount(mcrawPath, mountPath);
    if (mount) {
        m_mounts.push_back(mount);
        return mountPath;
    }
    return "";
}

bool McrawMountManager::isInsideMount(const std::string& path) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& m : m_mounts) {
        if (path.find(m->mountPath()) == 0) return true;
    }
    return false;
}

void McrawMountManager::unmountAll() {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_mounts.clear(); // shared_ptr destructors call unmount()
}
