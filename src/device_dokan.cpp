#define _CRT_SECURE_NO_WARNINGS
#include "device_dokan.h"
#include "app.h"

#include <dokan/dokan.h>
#include <dokan/fileinfo.h>

#include <thread>
#include <unordered_map>
#include <algorithm>
#include <filesystem>

struct WriteBuffer {
    std::string devicePath;
    std::vector<char> buffer;
    uint64_t fileOffset = 0;
    bool sequential = true;

    static constexpr size_t FLUSH_SIZE = 4 * 1024 * 1024; // 4MB

    void write(uint64_t offset, const void* data, uint32_t len) {
        if (buffer.empty()) {
            fileOffset = offset;
        }

        uint64_t bufEnd = fileOffset + buffer.size();
        if (offset == bufEnd) {
            buffer.insert(buffer.end(), (const char*)data, (const char*)data + len);
        } else if (offset >= fileOffset && offset + len <= bufEnd) {
            memcpy(buffer.data() + (offset - fileOffset), data, len);
        } else {
            sequential = false;
            buffer.assign((const char*)data, (const char*)data + len);
            fileOffset = offset;
        }
    }

    bool needsFlush() const { return buffer.size() >= FLUSH_SIZE || !sequential; }
};

// Global write buffer map — keyed by device path
static std::mutex g_wbMutex;
static std::unordered_map<std::string, std::unique_ptr<WriteBuffer>> g_writeBuffers;

struct DokanContext {
    DeviceClient* device = nullptr;
    std::string storageRoot;  // e.g. "/storage/emulated/0"

    // --- Directory listing cache ---
    struct CachedDir {
        std::vector<DeviceFileEntry> entries;
        std::chrono::steady_clock::time_point fetchTime;
    };
    std::mutex dirMutex;
    std::unordered_map<std::string, CachedDir> dirCache;

    const std::vector<DeviceFileEntry>* getDir(const std::string& path) {
        std::lock_guard<std::mutex> lk(dirMutex);
        auto now = std::chrono::steady_clock::now();
        auto it = dirCache.find(path);
        if (it != dirCache.end()) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.fetchTime).count();
            if (age < 5) return &it->second.entries;
        }
        auto entries = device->listDirectory(path);
        auto& cached = dirCache[path];
        cached.entries = std::move(entries);
        cached.fetchTime = now;
        return &cached.entries;
    }

    void invalidateDir(const std::string& path) {
        std::lock_guard<std::mutex> lk(dirMutex);
        dirCache.erase(path);
    }

    // Find a single entry by looking up parent dir (cached)
    bool findEntry(const std::string& devPath, DeviceFileEntry& out) {
        if (devPath == storageRoot) {
            out.name = "";
            out.type = 1; // dir
            out.size = 0;
            out.mtime = 0;
            return true;
        }
        auto slash = devPath.rfind('/');
        if (slash == std::string::npos) return false;
        std::string parentPath = devPath.substr(0, slash);
        std::string fileName = devPath.substr(slash + 1);
        const auto* entries = getDir(parentPath);
        if (!entries) return false;
        for (const auto& e : *entries) {
            if (e.name == fileName) { out = e; return true; }
        }
        return false;
    }

    // --- Read-ahead cache ---
    // Caches the last read chunk per file path to accelerate sequential reads.
    // When a read hits within the cached range, serve from cache.
    // When a read is sequential (starts at cache end), fetch a larger chunk.
    struct ReadCache {
        std::string path;
        uint64_t offset = 0;      // start of cached data
        std::vector<char> data;   // cached bytes
        std::chrono::steady_clock::time_point lastAccess;
    };
    static constexpr size_t READ_AHEAD_SIZE = 4 * 1024 * 1024; // 4MB read-ahead
    static constexpr int MAX_READ_CACHES = 4; // cache for up to 4 concurrent files
    std::mutex readMutex;
    std::vector<ReadCache> readCaches;

    // Try to serve a read from cache. Returns bytes served (0 = miss).
    uint64_t readFromCache(const std::string& path, uint64_t offset, uint32_t length, void* buffer) {
        std::lock_guard<std::mutex> lk(readMutex);
        for (auto& rc : readCaches) {
            if (rc.path == path &&
                offset >= rc.offset &&
                offset + length <= rc.offset + rc.data.size()) {
                // Cache hit
                size_t cacheOff = (size_t)(offset - rc.offset);
                memcpy(buffer, rc.data.data() + cacheOff, length);
                rc.lastAccess = std::chrono::steady_clock::now();
                return length;
            }
        }
        return 0;
    }

    // Store data in cache, evicting oldest if full
    void cacheRead(const std::string& path, uint64_t offset, const void* data, size_t len) {
        std::lock_guard<std::mutex> lk(readMutex);
        // Update existing cache for this path
        for (auto& rc : readCaches) {
            if (rc.path == path) {
                rc.offset = offset;
                rc.data.assign((const char*)data, (const char*)data + len);
                rc.lastAccess = std::chrono::steady_clock::now();
                return;
            }
        }
        // Evict oldest if full
        if ((int)readCaches.size() >= MAX_READ_CACHES) {
            auto oldest = std::min_element(readCaches.begin(), readCaches.end(),
                [](const ReadCache& a, const ReadCache& b) {
                    return a.lastAccess < b.lastAccess;
                });
            readCaches.erase(oldest);
        }
        ReadCache rc;
        rc.path = path;
        rc.offset = offset;
        rc.data.assign((const char*)data, (const char*)data + len);
        rc.lastAccess = std::chrono::steady_clock::now();
        readCaches.push_back(std::move(rc));
    }
};

static DokanContext g_ctx[2]; // one per device slot

// Get the DokanContext from a DOKAN_FILE_INFO (slot stored in GlobalContext)
static DokanContext& ctxFromInfo(PDOKAN_FILE_INFO info) {
    int slot = (int)(uintptr_t)info->DokanOptions->GlobalContext;
    return g_ctx[slot & 1];
}

// Shorthand: declare ctx from DokanFileInfo at top of each callback
#define CTX auto& ctx = ctxFromInfo(DokanFileInfo)

// Convert Dokan wide path (backslash-separated, relative to mount root) to device path
static std::string toDevicePath(LPCWSTR fileName, DokanContext& ctx) {
    if (!fileName || fileName[0] == L'\0' || (fileName[0] == L'\\' && fileName[1] == L'\0'))
        return ctx.storageRoot;

    int len = WideCharToMultiByte(CP_UTF8, 0, fileName, -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, fileName, -1, utf8.data(), len, nullptr, nullptr);
    for (char& c : utf8) if (c == '\\') c = '/';

    if (!utf8.empty() && utf8[0] == '/')
        return ctx.storageRoot + utf8;
    return ctx.storageRoot + "/" + utf8;
}

static std::wstring utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    std::wstring wide(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), wide.data(), len);
    return wide;
}

// Unix timestamp -> FILETIME
static FILETIME unixToFiletime(int64_t unixSec) {
    ULARGE_INTEGER uli;
    uli.QuadPart = ((uint64_t)unixSec + 11644473600ULL) * 10000000ULL;
    FILETIME ft;
    ft.dwLowDateTime = uli.LowPart;
    ft.dwHighDateTime = uli.HighPart;
    return ft;
}

static NTSTATUS DOKAN_CALLBACK FsCreateFile(
    LPCWSTR FileName, PDOKAN_IO_SECURITY_CONTEXT, ACCESS_MASK DesiredAccess,
    ULONG FileAttributes, ULONG, ULONG CreateDisposition, ULONG CreateOptions,
    PDOKAN_FILE_INFO DokanFileInfo)
{
    CTX;
    std::string devPath = toDevicePath(FileName, ctx);

    bool isWrite = (CreateDisposition == FILE_CREATE ||
                    CreateDisposition == FILE_OVERWRITE ||
                    CreateDisposition == FILE_OVERWRITE_IF ||
                    CreateDisposition == FILE_SUPERSEDE ||
                    (DesiredAccess & (FILE_WRITE_DATA | FILE_APPEND_DATA)));

    // Look up entry via cached directory listing
    DeviceFileEntry entry;
    bool exists = ctx.findEntry(devPath, entry);
    bool isDir = exists && entry.isDirectory();

    if (isDir) {
        DokanFileInfo->IsDirectory = TRUE;
        if (CreateOptions & FILE_NON_DIRECTORY_FILE)
            return STATUS_FILE_IS_A_DIRECTORY;
        return STATUS_SUCCESS;
    }

    if (exists && (CreateOptions & FILE_DIRECTORY_FILE))
        return STATUS_NOT_A_DIRECTORY;

    // Handle directory creation
    if (!exists && (CreateOptions & FILE_DIRECTORY_FILE)) {
        DeviceMountManager::instance().ioActive = true;
        bool ok = ctx.device->createDirectory(devPath);
        DeviceMountManager::instance().ioActive = false;
        if (!ok) return STATUS_ACCESS_DENIED;
        DokanFileInfo->IsDirectory = TRUE;
        auto s = devPath.rfind('/');
        if (s != std::string::npos)
            ctx.invalidateDir(devPath.substr(0, s));
        return STATUS_SUCCESS;
    }

    // Handle write/create
    if (isWrite) {
        if (!exists && CreateDisposition != FILE_CREATE &&
            CreateDisposition != FILE_OVERWRITE_IF &&
            CreateDisposition != FILE_SUPERSEDE) {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }

        // Create/truncate file on device if needed
        if (!exists || CreateDisposition == FILE_OVERWRITE ||
            CreateDisposition == FILE_OVERWRITE_IF ||
            CreateDisposition == FILE_SUPERSEDE) {
            ctx.device->createFile(devPath, 0);
            auto s = devPath.rfind('/');
            if (s != std::string::npos)
                ctx.invalidateDir(devPath.substr(0, s));
        }

        // Register write buffer for batching
        {
            std::lock_guard<std::mutex> lk(g_wbMutex);
            auto wb = std::make_unique<WriteBuffer>();
            wb->devicePath = devPath;
            g_writeBuffers[devPath] = std::move(wb);
        }

        return STATUS_SUCCESS;
    }

    if (!exists)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK FsDeleteFile(LPCWSTR FileName, PDOKAN_FILE_INFO DokanFileInfo) {
    CTX;
    // Just check if the file exists — actual deletion happens in Cleanup
    std::string devPath = toDevicePath(FileName, ctx);
    DeviceFileEntry entry;
    if (!ctx.findEntry(devPath, entry)) return STATUS_OBJECT_NAME_NOT_FOUND;
    return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK FsDeleteDirectory(LPCWSTR FileName, PDOKAN_FILE_INFO DokanFileInfo) {
    CTX;
    std::string devPath = toDevicePath(FileName, ctx);
    DeviceFileEntry entry;
    if (!ctx.findEntry(devPath, entry)) return STATUS_OBJECT_NAME_NOT_FOUND;
    if (!entry.isDirectory()) return STATUS_NOT_A_DIRECTORY;
    return STATUS_SUCCESS;
}

// Flush a write buffer to the device (caller holds g_wbMutex or has exclusive access)
static void flushWriteBuffer(DokanContext& ctx, WriteBuffer* wb) {
    if (!wb || wb->buffer.empty()) return;
    DeviceMountManager::instance().ioActive = true;
    ctx.device->writeRange(wb->devicePath, wb->fileOffset,
                              wb->buffer.data(), (uint32_t)wb->buffer.size());
    DeviceMountManager::instance().ioActive = false;
    wb->buffer.clear();
}

// Flush and remove write buffer for a device path
static void flushAndRemoveWriteBuffer(DokanContext& ctx, const std::string& devPath) {
    std::unique_ptr<WriteBuffer> wb;
    {
        std::lock_guard<std::mutex> lk(g_wbMutex);
        auto it = g_writeBuffers.find(devPath);
        if (it == g_writeBuffers.end()) return;
        wb = std::move(it->second);
        g_writeBuffers.erase(it);
    }
    flushWriteBuffer(ctx, wb.get());
}

static void DOKAN_CALLBACK FsCleanup(LPCWSTR FileName, PDOKAN_FILE_INFO DokanFileInfo) {
    CTX;
    // Flush remaining write buffer for this file
    if (FileName) {
        std::string devPath = toDevicePath(FileName, ctx);
        flushAndRemoveWriteBuffer(ctx, devPath);
        // Invalidate parent dir cache
        auto slash = devPath.rfind('/');
        if (slash != std::string::npos)
            ctx.invalidateDir(devPath.substr(0, slash));
    }

    // Handle delete on close
    if (DokanFileInfo->DeletePending && FileName) {
        std::string devPath = toDevicePath(FileName, ctx);
        LOG_INFO("Dokan", "Deleting: " + devPath);
        DeviceMountManager::instance().ioActive = true;
        ctx.device->deleteFile(devPath);
        DeviceMountManager::instance().ioActive = false;
        auto slash = devPath.rfind('/');
        if (slash != std::string::npos)
            ctx.invalidateDir(devPath.substr(0, slash));
    }
}

static void DOKAN_CALLBACK FsCloseFile(LPCWSTR, PDOKAN_FILE_INFO DokanFileInfo) {
    DokanFileInfo->Context = 0;
}

static NTSTATUS DOKAN_CALLBACK FsSetEndOfFile(
    LPCWSTR FileName, LONGLONG ByteOffset, PDOKAN_FILE_INFO) {
    // Accept silently — the file on device will be the right size after writes
    return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK FsSetAllocationSize(
    LPCWSTR FileName, LONGLONG AllocSize, PDOKAN_FILE_INFO) {
    return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK FsSetFileAttributes(
    LPCWSTR FileName, DWORD FileAttributes, PDOKAN_FILE_INFO) {
    // Android doesn't have Windows file attributes — accept silently
    return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK FsSetFileTime(
    LPCWSTR FileName, CONST FILETIME* CreationTime,
    CONST FILETIME* LastAccessTime, CONST FILETIME* LastWriteTime,
    PDOKAN_FILE_INFO) {
    // Accept silently — we don't implement utime on device
    return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK FsMoveFile(
    LPCWSTR FileName, LPCWSTR NewFileName, BOOL ReplaceIfExisting,
    PDOKAN_FILE_INFO DokanFileInfo) {
    CTX;
    std::string oldPath = toDevicePath(FileName, ctx);
    std::string newPath = toDevicePath(NewFileName, ctx);

    DeviceMountManager::instance().ioActive = true;
    bool ok = ctx.device->renameFile(oldPath, newPath);
    DeviceMountManager::instance().ioActive = false;

    if (!ok) return STATUS_ACCESS_DENIED;

    // Invalidate both parent dir caches
    auto s1 = oldPath.rfind('/');
    auto s2 = newPath.rfind('/');
    if (s1 != std::string::npos) ctx.invalidateDir(oldPath.substr(0, s1));
    if (s2 != std::string::npos) ctx.invalidateDir(newPath.substr(0, s2));

    return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK FsWriteFile(
    LPCWSTR FileName, LPCVOID Buffer, DWORD NumberOfBytesToWrite,
    LPDWORD NumberOfBytesWritten, LONGLONG Offset, PDOKAN_FILE_INFO DokanFileInfo)
{
    CTX;
    std::string devPath = toDevicePath(FileName, ctx);

    // Try buffered write
    {
        std::lock_guard<std::mutex> lk(g_wbMutex);
        auto it = g_writeBuffers.find(devPath);
        if (it != g_writeBuffers.end()) {
            auto* wb = it->second.get();

            // Flush if needed before writing
            if (wb->needsFlush() && !wb->buffer.empty()) {
                DokanResetTimeout(60000, DokanFileInfo);
                flushWriteBuffer(ctx, wb);
            }

            wb->write((uint64_t)Offset, Buffer, NumberOfBytesToWrite);

            if (wb->needsFlush()) {
                DokanResetTimeout(60000, DokanFileInfo);
                flushWriteBuffer(ctx, wb);
            }

            *NumberOfBytesWritten = NumberOfBytesToWrite;
            return STATUS_SUCCESS;
        }
    }

    // Fallback: direct write
    DeviceMountManager::instance().ioActive = true;
    DokanResetTimeout(60000, DokanFileInfo);

    uint64_t written = ctx.device->writeRange(devPath, (uint64_t)Offset,
                                                 Buffer, NumberOfBytesToWrite);
    DeviceMountManager::instance().ioActive = false;

    if (written == 0) {
        *NumberOfBytesWritten = 0;
        return STATUS_ACCESS_DENIED;
    }

    *NumberOfBytesWritten = (DWORD)written;
    return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK FsReadFile(
    LPCWSTR FileName, LPVOID Buffer, DWORD BufferLength,
    LPDWORD ReadLength, LONGLONG Offset, PDOKAN_FILE_INFO DokanFileInfo)
{
    CTX;
    std::string devPath = toDevicePath(FileName, ctx);

    // Try cache first
    uint64_t cached = ctx.readFromCache(devPath, (uint64_t)Offset, BufferLength, Buffer);
    if (cached > 0) {
        *ReadLength = (DWORD)cached;
        return STATUS_SUCCESS;
    }

    DeviceMountManager::instance().ioActive = true;
    DokanResetTimeout(60000, DokanFileInfo);

    // Fetch a larger chunk than requested for read-ahead
    // This dramatically reduces round-trips for sequential reads
    uint64_t fetchSize = (BufferLength > DokanContext::READ_AHEAD_SIZE) ? BufferLength : DokanContext::READ_AHEAD_SIZE;
    std::vector<char> fetchBuf(fetchSize);

    uint64_t bytesRead = ctx.device->readRange(devPath, (uint64_t)Offset, (uint64_t)fetchSize, fetchBuf.data());

    DeviceMountManager::instance().ioActive = false;

    if (bytesRead == 0) {
        *ReadLength = 0;
        return STATUS_END_OF_FILE;
    }

    // Cache the full fetched chunk
    ctx.cacheRead(devPath, (uint64_t)Offset, fetchBuf.data(), (size_t)bytesRead);

    // Return only what was requested
    uint32_t toReturn = (uint32_t)(BufferLength < bytesRead ? BufferLength : bytesRead);
    memcpy(Buffer, fetchBuf.data(), toReturn);
    *ReadLength = toReturn;
    return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK FsGetFileInformation(
    LPCWSTR FileName, LPBY_HANDLE_FILE_INFORMATION Buffer, PDOKAN_FILE_INFO DokanFileInfo)
{
    CTX;
    std::string devPath = toDevicePath(FileName, ctx);
    memset(Buffer, 0, sizeof(*Buffer));

    DeviceFileEntry e;
    if (!ctx.findEntry(devPath, e))
        return STATUS_OBJECT_NAME_NOT_FOUND;

    Buffer->dwFileAttributes = e.isDirectory() ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    if (!e.isDirectory()) {
        Buffer->nFileSizeLow = (DWORD)(e.size & 0xFFFFFFFF);
        Buffer->nFileSizeHigh = (DWORD)(e.size >> 32);
    }
    FILETIME ft = unixToFiletime(e.mtime);
    Buffer->ftCreationTime = ft;
    Buffer->ftLastAccessTime = ft;
    Buffer->ftLastWriteTime = ft;
    Buffer->nNumberOfLinks = 1;
    return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK FsFindFiles(
    LPCWSTR FileName, PFillFindData FillFindData, PDOKAN_FILE_INFO DokanFileInfo)
{
    CTX;
    std::string devPath = toDevicePath(FileName, ctx);
    const auto* entries = ctx.getDir(devPath);
    if (!entries) return STATUS_OBJECT_PATH_NOT_FOUND;

    for (const auto& e : *entries) {
        WIN32_FIND_DATAW findData = {};
        std::wstring wideName = utf8ToWide(e.name);
        wcsncpy_s(findData.cFileName, wideName.c_str(), MAX_PATH - 1);

        findData.dwFileAttributes = e.isDirectory() ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        if (!e.isDirectory()) {
            findData.nFileSizeLow = (DWORD)(e.size & 0xFFFFFFFF);
            findData.nFileSizeHigh = (DWORD)(e.size >> 32);
        }
        FILETIME ft = unixToFiletime(e.mtime);
        findData.ftCreationTime = ft;
        findData.ftLastAccessTime = ft;
        findData.ftLastWriteTime = ft;

        FillFindData(&findData, DokanFileInfo);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK FsGetVolumeInformation(
    LPWSTR VolumeNameBuffer, DWORD VolumeNameSize,
    LPDWORD VolumeSerialNumber, LPDWORD MaximumComponentLength,
    LPDWORD FileSystemFlags, LPWSTR FileSystemNameBuffer,
    DWORD FileSystemNameSize, PDOKAN_FILE_INFO)
{
    wcscpy_s(VolumeNameBuffer, VolumeNameSize, L"Android");
    *VolumeSerialNumber = 0xAFAF0001;
    *MaximumComponentLength = 255;
    *FileSystemFlags = FILE_CASE_SENSITIVE_SEARCH | FILE_CASE_PRESERVED_NAMES;
    wcscpy_s(FileSystemNameBuffer, FileSystemNameSize, L"FastEnoughFS");
    return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK FsGetDiskFreeSpace(
    PULONGLONG FreeBytesAvailable, PULONGLONG TotalNumberOfBytes,
    PULONGLONG TotalNumberOfFreeBytes, PDOKAN_FILE_INFO DokanFileInfo)
{
    CTX;
    uint64_t total = 0, free = 0;
    if (ctx.device->getDiskSpace(total, free)) {
        *TotalNumberOfBytes = total;
        *FreeBytesAvailable = free;
        *TotalNumberOfFreeBytes = free;
    } else {
        // Fallback if command not supported (old server)
        *TotalNumberOfBytes = 512ULL * 1024 * 1024 * 1024;
        *FreeBytesAvailable = 128ULL * 1024 * 1024 * 1024;
        *TotalNumberOfFreeBytes = 128ULL * 1024 * 1024 * 1024;
    }
    return STATUS_SUCCESS;
}

DeviceMountManager& DeviceMountManager::instance(int slot) {
    static DeviceMountManager s[2];
    s[0].m_slot = 0;
    s[1].m_slot = 1;
    return s[slot & 1];
}

bool DeviceMountManager::mount(DeviceClient* device, const std::string& storageRoot,
                                const std::string& mountPoint) {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_mounted) return false;

    g_ctx[m_slot].device = device;
    g_ctx[m_slot].storageRoot = storageRoot;
    m_mountPoint = mountPoint;

    // Convert mount point to wide string
    std::wstring wideMountPoint = utf8ToWide(mountPoint);

    m_thread = std::thread([this, wideMountPoint]() {
        DokanInit();

        DOKAN_OPTIONS options = {};
        options.Version = DOKAN_VERSION;
        options.MountPoint = wideMountPoint.c_str();
        options.Options = DOKAN_OPTION_CURRENT_SESSION;
        options.SingleThread = FALSE;
        options.Timeout = 60000; // 60s timeout
        options.GlobalContext = (ULONG64)m_slot;

        DOKAN_OPERATIONS ops = {};
        ops.ZwCreateFile = FsCreateFile;
        ops.Cleanup = FsCleanup;
        ops.CloseFile = FsCloseFile;
        ops.ReadFile = FsReadFile;
        ops.WriteFile = FsWriteFile;
        ops.DeleteFile = FsDeleteFile;
        ops.DeleteDirectory = FsDeleteDirectory;
        ops.SetEndOfFile = FsSetEndOfFile;
        ops.SetAllocationSize = FsSetAllocationSize;
        ops.SetFileAttributes = FsSetFileAttributes;
        ops.SetFileTime = FsSetFileTime;
        ops.MoveFile = FsMoveFile;
        ops.GetFileInformation = FsGetFileInformation;
        ops.FindFiles = FsFindFiles;
        ops.GetVolumeInformation = FsGetVolumeInformation;
        ops.GetDiskFreeSpace = FsGetDiskFreeSpace;

        DOKAN_HANDLE instance = nullptr;
        int result = DokanCreateFileSystem(&options, &ops, &instance);

        if (result == DOKAN_SUCCESS) {
            m_dokanInstance = instance;
            m_mounted = true;
            LOG_INFO("Dokan", "Mounted: " + g_ctx[m_slot].storageRoot + " -> " + m_mountPoint);

            // Wait until unmount
            DokanWaitForFileSystemClosed(instance, INFINITE);
            DokanCloseHandle(instance);
        } else {
            LOG_ERROR("Dokan", "Mount failed with code " + std::to_string(result));
        }

        m_mounted = false;
        m_dokanInstance = nullptr;
        DokanShutdown();
    });

    // Wait briefly for mount to succeed
    for (int i = 0; i < 20 && !m_mounted; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return m_mounted;
}

void DeviceMountManager::unmount() {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_mounted) return;

    // Signal Dokan to unmount — this unblocks DokanWaitForFileSystemClosed
    // in the thread, which then calls DokanCloseHandle itself
    std::wstring wideMountPoint(m_mountPoint.begin(), m_mountPoint.end());
    DokanRemoveMountPoint(wideMountPoint.c_str());

    m_mounted = false;
    m_dokanInstance = nullptr;

    if (m_thread.joinable()) {
        m_thread.join();
    }

    LOG_INFO("Dokan", "Unmounted: " + m_mountPoint);
}
