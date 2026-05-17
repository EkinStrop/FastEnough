#pragma once
#include "device_client.h"
#include "imgui.h"
#include <string>
#include <vector>
#include <memory>
#include <deque>
#include <set>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <filesystem>
#include <chrono>
#include <ctime>

// ---- Debug Log System ----
enum class LogLevel { Debug, Info, Warn, Error };

struct LogEntry {
    std::chrono::steady_clock::time_point time;
    std::chrono::system_clock::time_point wallTime; // real clock time for display
    LogLevel level;
    std::string tag;
    std::string message;
};

class DebugLog {
public:
    static DebugLog& instance() { static DebugLog s; return s; }

    void log(LogLevel level, const std::string& tag, const std::string& msg) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_entries.push_back({ std::chrono::steady_clock::now(), std::chrono::system_clock::now(), level, tag, msg });
        if (m_entries.size() > 5000) m_entries.erase(m_entries.begin(), m_entries.begin() + 1000);
        m_scrollToBottom = true;
    }

    void clear() { std::lock_guard<std::mutex> lk(m_mutex); m_entries.clear(); }

    // Returns a snapshot for rendering (thread-safe)
    std::vector<LogEntry> snapshot() {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_entries;
    }

    bool m_scrollToBottom = false;

private:
    DebugLog() = default;
    std::mutex m_mutex;
    std::vector<LogEntry> m_entries;
};

// Convenience macros
#define LOG_DEBUG(tag, msg) DebugLog::instance().log(LogLevel::Debug, tag, msg)
#define LOG_INFO(tag, msg)  DebugLog::instance().log(LogLevel::Info,  tag, msg)
#define LOG_WARN(tag, msg)  DebugLog::instance().log(LogLevel::Warn,  tag, msg)
#define LOG_ERROR(tag, msg) DebugLog::instance().log(LogLevel::Error, tag, msg)

// Taskbar progress (implemented in main.cpp)
void updateTaskbarProgress(float fraction, bool active);
void setTaskbarError();
void setTaskbarPaused();

struct WindowsFileEntry {
    std::string name;
    uint64_t size = 0;
    std::string dateModified;
    bool isDirectory = false;
    bool isHidden = false;
};

// --- Transfer Batch System ---

struct BatchFileItem {
    std::string sourcePath;
    std::string destPath;
    std::string displayName;
    uint64_t fileSize = 0;
    bool isDirectory = false;

    // MCRAW virtual item fields
    bool isMcrawVirtual = false;
    std::string mcrawPath;      // path to the .mcraw container
    std::string virtualName;    // e.g. "frame_000001.dng"
};

enum class BatchState { Queued, Running, Paused, Verifying, Completed, Failed, Stopped, WaitingConflict };

// User's choice when a destination file already exists
enum class ConflictAction { None, Overwrite, Skip, OverwriteAll, SkipAll };

struct TransferBatch {
    std::vector<BatchFileItem> files;
    bool isPull = true;      // Android -> Windows
    bool isLocalCopy = false; // Windows -> Windows (no device involved)
    bool isMove = false;     // move (delete source after transfer) vs copy
    bool isCrossDevice = false; // Android-to-Android across two devices
    int srcDeviceSlot = 0;   // device slot for source (Android panels)
    int dstDeviceSlot = 0;   // device slot for destination (Android panels)
    bool useDokanRelay = false; // true=stream via Dokan, false=pull to temp then push
    bool useParallelChannels = false; // use both USB+WiFi channels simultaneously

    // Per-channel tracking for parallel transfers
    struct ChannelProgress {
        std::atomic<uint64_t> bytesTransferred{0};
        std::atomic<uint64_t> totalBytes{0};
        std::atomic<float> progress{0.0f};
        std::atomic<double> speed{0.0};
        std::atomic<int> filesCompleted{0};
        int filesAssigned = 0;
        std::string channelName;
        std::string currentFile;
        std::atomic<double> elapsedSec{0.0};
        std::atomic<uint64_t> curBlockSize{0};        // size of current block being transferred
        std::atomic<uint64_t> curBlockTransferred{0}; // bytes done in current block
    };
    static constexpr int MAX_CHANNELS = 8;
    ChannelProgress channels[MAX_CHANNELS];
    int numChannels = 2;   // how many channels are active (default 2 for dual-channel compat)
    bool useMultiNic = false; // true = multi-NIC, false = legacy dual-channel

    // Overall tracking
    std::atomic<int> currentFileIndex{0};       // index of file being transferred
    std::atomic<uint64_t> totalBytes{0};         // sum of all file sizes
    std::atomic<uint64_t> totalTransferred{0};   // bytes done across all files
    std::atomic<float> totalProgress{0.0f};      // 0-1

    // Current file tracking
    std::atomic<uint64_t> curFileSize{0};
    std::atomic<uint64_t> curFileTransferred{0};
    std::atomic<float> curFileProgress{0.0f};

    // Speed / ETA (for the whole batch)
    std::atomic<double> speedBytesPerSec{0.0};
    std::atomic<double> etaSeconds{-1.0};

    // State
    std::atomic<BatchState> state{BatchState::Queued};
    std::atomic<bool> pauseRequested{false};
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> disconnected{false};  // set when connection lost mid-transfer
    std::atomic<bool> waitingForUserRetry{false}; // waiting for user to click retry or cancel
    std::atomic<bool> userRetryRequested{false};
    std::string errorMessage;

    // File conflict resolution
    std::atomic<bool> waitingConflict{false};     // batch thread is waiting for user's conflict decision
    std::atomic<ConflictAction> conflictResponse{ConflictAction::None}; // UI thread sets this
    std::string conflictFileName;                  // name of the conflicting file (for display)
    ConflictAction conflictAllDecision = ConflictAction::None; // sticky "All" choice for this batch
    int skippedFiles = 0;                          // count of files skipped due to conflicts
    std::atomic<int> errorSkippedFiles{0};          // count of files skipped due to errors (ENOENT, etc.)
    std::vector<std::string> errorSkippedNames;     // display names of error-skipped files
    std::mutex errorSkippedMutex;

    // Timing
    std::chrono::steady_clock::time_point startTime;
    std::atomic<double> pausedSeconds{0.0};  // total time spent paused/disconnected (excluded from avg speed)
    std::atomic<double> finalAvgSpeed{0.0};
    std::atomic<double> finalTimeSec{0.0};

    // CRC verification progress (two phases: remote then local)
    std::atomic<float> crcProgress{0.0f};  // 0-1, updated during local CRC computation
    std::atomic<int> crcPhase{0};          // 0=idle, 1=remote (server computing), 2=local (client computing)
    std::string crcFileName;               // file currently being verified

    // CRC results per file
    struct CrcResult {
        std::string fileName;
        bool passed = false;
        std::string detail; // e.g. "Local: AABBCCDD  Remote: AABBCCDD" or error reason
        double remoteMs = 0;  // server-side CRC time (ms)
        double localMs = 0;   // client-side CRC time (ms)
        double totalMs = 0;   // total verification time (ms)
        uint64_t fileSize = 0; // file size for throughput calculation
        std::string sourcePath; // original source path (for retry)
        std::string destPath;   // original dest path (for retry)
    };
    std::vector<CrcResult> crcResults;
    int crcPassCount() const { int n=0; for (auto& r:crcResults) if (r.passed) n++; return n; }
    int crcFailCount() const { int n=0; for (auto& r:crcResults) if (!r.passed) n++; return n; }

    int completedFiles() const { return currentFileIndex.load(); }
    int totalFiles() const { return (int)files.size(); }
};

// --- Drag payload ---
struct DragPayload {
    bool isAndroid;
};

enum class PanelSide { Left, Right };

struct FilePanel {
    std::string currentPath;
    std::vector<WindowsFileEntry> windowsEntries;
    std::vector<DeviceFileEntry> androidEntries;
    std::set<int> selectedIndices;
    int focusedIndex = -1;
    bool isAndroid = false;
    bool needsRefresh = true;
    float scrollY = 0.0f;
    char pathInput[1024] = {};
    char searchFilter[256] = {};
    bool editingPath = false;
    bool showHidden = false;

    // MCRAW virtual directory state
    bool insideMcraw = false;
    std::string mcrawFilePath;  // path to the .mcraw container we're browsing

    // Sorting: 0=name, 1=size, 2=date. Negative = descending
    int sortColumn = 0;
    bool sortDescending = false;

    // Rubber-band (lasso) selection
    bool rubberBandActive = false;
    ImVec2 rubberBandStart;
    ImVec2 rubberBandEnd;

    // Device assignment (for Android panels) — 0=primary, 1=secondary
    int deviceSlot = 0;

    // Navigation history
    std::vector<std::string> navHistory;
    int navHistoryPos = -1;

    int entryCount() const {
        return isAndroid ? (int)androidEntries.size() : (int)windowsEntries.size();
    }
    bool validIndex(int i) const {
        return i >= 0 && i < entryCount();
    }
    std::string entryName(int i) const {
        if (!validIndex(i)) return "";
        return isAndroid ? androidEntries[i].name : windowsEntries[i].name;
    }
    bool entryIsDir(int i) const {
        if (!validIndex(i)) return false;
        return isAndroid ? androidEntries[i].isDirectory() : windowsEntries[i].isDirectory;
    }
    uint64_t entrySize(int i) const {
        if (!validIndex(i)) return 0;
        return isAndroid ? androidEntries[i].size : windowsEntries[i].size;
    }
    std::string entryDate(int i) const {
        if (!validIndex(i)) return "";
        if (isAndroid) {
            // Convert mtime to string
            time_t t = (time_t)androidEntries[i].mtime;
            struct tm tm; localtime_s(&tm, &t);
            char buf[32]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
            return buf;
        }
        return windowsEntries[i].dateModified;
    }
};

// --- App Preferences (persisted to disk) ---
// Saved WiFi ADB device for auto-reconnect
struct SavedWifiDevice {
    std::string serial;   // USB serial (to match when USB is connected)
    std::string wifiIp;   // WiFi IP for adb connect
    int port = 5555;      // WiFi ADB port
    bool autoConnect = true;
    std::string model;    // Phone model name for display
};

// Saved NIC binding for multi-NIC parallel transfers
struct NicBinding {
    std::string adapterName;  // friendly name, e.g. "Ethernet 2"
    std::string localIp;      // IPv4 address on this NIC
    bool enabled = true;
};

// Detected NIC info for the configuration UI
struct DetectedNic {
    std::string adapterName;
    std::string ipAddress;
    std::string description;
    uint64_t speed = 0;  // link speed in bps
};

struct AppPreferences {
    bool restartAdbOnLaunch = false;  // default OFF
    bool enableCrcVerification = true;
    bool autoDismissTransfer = false; // auto-close transfer overlay on success
    bool wifiAutoConnect = false;    // auto-setup WiFi ADB when USB connects (off until wizard completes)
    bool confirmOnClose = true;      // show yes/no dialog when pressing X (default ON)
    bool killAdbOnClose = true;      // kill ADB server when app exits (default ON)
    std::vector<SavedWifiDevice> savedWifiDevices;
    bool enableMultiNic = false;
    std::vector<NicBinding> multiNicBindings;
    int usbPipeCount = 2;   // 1 = single, 2 = dual ADB forward pipes (2x USB throughput)
    int wifiPipeCount = 1;  // 1 = single, 2 = dual WiFi TCP connections
    bool useRoot = false;   // launch the on-device server via `su -c` for restricted-path access

    void save();
    void load();
};

// --- Theme (persisted to disk) ---
struct AppTheme {
    float userScale = 1.0f;  // user scale multiplier on top of system DPI
    ImVec4 colors[ImGuiCol_COUNT] = {};
    bool customColors = false; // false = use built-in dark theme

    void save();
    void load();
    static std::string getThemePath();
};

class App {
public:
    App();
    ~App();

    void render();
    void applyScale(float newScale);
    AppPreferences m_prefs;
    AppTheme m_theme;
    float m_systemDpiScale = 1.0f;  // set from main.cpp before first render
    float m_pendingScale = -1.0f;   // >0 means applyScale deferred to next frame
    bool m_themeNeedsReposition = false; // force re-center theme window after scale change
    float m_scalePreview = -1.0f;       // slider preview value (not yet applied)

    // External drag-and-drop (Explorer <-> App)
    void handleExternalFileDrop(const std::vector<std::string>& paths, int mouseX, int mouseY);
    void performOleDragDrop(const std::vector<std::string>& filePaths);
    void performAndroidDragOut(FilePanel* src);
    bool m_pendingOleDrag = false;
    std::vector<std::string> m_oleDragPaths;
    bool m_isDragging = false;
    FilePanel* m_dragSourcePanel = nullptr;
    // Number of in-flight Android drag streams that may currently hold the
    // device mutex via readRangeStreaming. Used to skip panel refresh so the
    // UI thread does not block waiting for a still-draining producer after
    // the user cancels an Explorer copy.
    std::atomic<int> m_androidDragInflight{0};
    bool m_showCloseConfirm = false;  // close confirmation dialog

private:
    void setupStyle();
    void renderMenuBar();
    void renderPreferencesWindow();
    void renderCloseConfirmDialog();
    void renderDeviceBar();
    void renderPanel(FilePanel& panel, PanelSide side);
    void renderTransferOverlay();
    void renderStatusBar();
    void renderContextMenu(FilePanel& panel);
    void renderNewFolderPopup();
    void renderRenamePopup();
    void renderDeleteConfirmPopup();
    void renderAboutPopup();
    void renderNotificationPopup();
    void renderThemeWindow();
    void renderNicConfigWindow();
    void renderWifiWizard();
    void renderWifiPairingDialog();
    void renderWifiBanner();
    void tryAutoWifiConnect(const std::string& serial);
    void renderCopyMoveDialog();
    void renderCrossDeviceDialog();
    void openAndroidFile(FilePanel& panel, int index);

    void refreshWindowsPanel(FilePanel& panel);
    void refreshAndroidPanel(FilePanel& panel);
    void navigateToDirectory(FilePanel& panel, const std::string& path);
    void navigateUp(FilePanel& panel);

    // Create a batch from selected files in srcPanel, targeting dstPanel
    void switchPanelMode(FilePanel& panel, bool toAndroid);
    void forEachAndroidPanel(std::function<void(FilePanel&)> fn);
    void startTransfer(bool pullFromAndroid);
    void startTransferFromDrag(FilePanel& srcPanel, FilePanel& dstPanel);
    void processBatchQueue();

    std::string queryDeviceDisplayName(const std::string& serial);
    static std::string formatSize(uint64_t bytes);
    static std::string formatSpeed(double bytesPerSec);
    static std::string formatETA(double seconds);
    static std::string getFileIcon(const std::string& name, bool isDir);
    std::vector<std::string> getWindowsDrives();

    // Draw a centered-text progress bar
    void drawProgressBar(float fraction, const char* overlayText, float height,
                         ImVec4 barColor, ImVec4 bgColor);

    void onDeviceChanged();

    // Dual-device support: slot 0 = primary, slot 1 = secondary
    DeviceClient m_deviceSlots[2];
    std::string m_slotSerial[2];       // serial assigned to each slot
    std::string m_slotStorageRoot[2];  // storage root per slot
    std::vector<std::string> m_slotVolumes[2]; // volumes per slot
    bool m_slotConnected[2] = {false, false};

    // Legacy accessors (slot 0 = primary for backward compat)
    DeviceClient& m_device = m_deviceSlots[0]; // MSVC handles this fine in practice
    std::string m_androidStorageRoot;           // kept in sync with m_slotStorageRoot[0]
    std::vector<std::string> m_androidVolumes;  // kept in sync with m_slotVolumes[0]

    std::vector<DeviceInfo> m_devices;
    int m_selectedDevice = -1;
    std::string m_lastDeviceSerial;

    // USB ↔ WiFi serial mapping (same physical device)
    // Key: USB serial, Value: WiFi serial (IP:port)
    std::string m_usbToWifiSerial;  // e.g. "3ca6acdb" → "192.168.178.192:5555"
    std::string m_wifiToUsbSerial;  // reverse mapping

    // Get the DeviceClient for a given panel
    DeviceClient& deviceFor(FilePanel& panel) { return m_deviceSlots[panel.deviceSlot]; }
    DeviceClient& deviceForSlot(int slot) { return m_deviceSlots[slot & 1]; }

    // Parallel transfer: additional channels to same device
    DeviceClient m_secondaryChannel;
    struct ExtraChannel {
        std::unique_ptr<DeviceClient> dev;
        bool isUsb; // true=USB ADB forward, false=WiFi Direct
    };
    std::vector<ExtraChannel> m_extraChannels; // for 3+ channel configs
    bool m_dualChannelAvailable = false;
    std::string m_secondaryChannelType; // "WiFi", "USB Tethering", or "USB"
    int m_activeChannelCount = 1; // total channels (primary + extras)

    // Multi-NIC parallel transfer
    std::vector<std::unique_ptr<DeviceClient>> m_nicChannels;
    std::vector<std::string> m_nicLocalIps;
    bool m_multiNicAvailable = false;

    FilePanel m_leftPanel;
    FilePanel m_rightPanel;

    // Batch queue
    std::deque<std::shared_ptr<TransferBatch>> m_batchQueue;
    std::mutex m_batchMutex;
    std::condition_variable m_batchCV;
    std::thread m_batchThread;
    std::atomic<bool> m_shutdownTransfer{false};

    // Overlay state
    bool m_overlayVisible = false;
    bool m_overlayWasOpen = false;

    // Notification popup (for important user-facing messages)
    bool m_showNotification = false;
    std::string m_notificationTitle;
    std::string m_notificationMessage;
    bool m_notificationIsError = false;

    // Connection mode UI
    bool m_showConnectionPopup = false;
    bool m_connectionPopupScanned = false;
    bool m_preferAdbForward = true; // user preference: default ADB Forward, Direct TCP is opt-in
    struct DetectedIp { std::string iface; std::string label; std::string ip; };
    std::vector<DetectedIp> m_detectedIps;
    char m_manualIp[64] = {};

    // Cached marketing names for device bar display (serial -> display name)
    std::unordered_map<std::string, std::string> m_deviceDisplayNames;

    bool m_showAboutPopup = false;
    bool m_showPreferences = false;
    bool m_showThemeWindow = false;
    bool m_showNicConfig = false;
    std::vector<DetectedNic> m_detectedNics;
    std::vector<bool> m_nicTestResults;      // per-NIC connectivity test result
    std::vector<bool> m_nicTestDone;         // whether test has been run
    std::vector<std::string> m_nicTestReason; // failure reason per NIC

    // WiFi ADB
    bool m_showWifiWizard = false;
    bool m_showWifiPairing = false;
    bool m_showMdnsDiscovery = false;
    int m_wizardStep = 0;           // 0=USB check, 1=WiFi check, 2=setting up, 3=done
    std::string m_wizardStatus;
    std::string m_wizardWifiIp;
    std::string m_wizardSerial;     // which device the wizard is operating on
    bool m_wizardBusy = false;
    char m_pairingIp[64] = {};
    char m_pairingCode[16] = {};
    char m_connectIp[64] = {};  // connect IP:port (different from pairing port)
    bool m_pairingDone = false; // show connect step after pairing
    bool m_wifiBannerDismissed = false;  // user dismissed the "enable dual channel?" banner
    bool m_wifiBannerShown = false;
    bool m_wifiAutoSetupDone = false;    // already auto-setup dual channel this session

    // Keep awake (wakelock held by a background adb shell process, auto-releases on disconnect)
    bool m_keepAwake = false;
    void* m_wakeLockProcess = nullptr;   // HANDLE to background adb shell process

    // WiFi auto-connect probing state (backoff: 5s x10, 30s x20, then 5min)
    int m_wifiProbeAttempts = 0;
    std::chrono::steady_clock::time_point m_lastWifiProbeTime{};

    // Copy/Move dialog
    bool m_showCopyMoveDialog = false;
    bool m_showCrossDeviceDialog = false;
    std::shared_ptr<TransferBatch> m_pendingBatch; // batch waiting for user decision
    bool m_showNewFolderPopup = false;
    bool m_showRenamePopup = false;
    bool m_newFolderNeedsFocus = false;
    bool m_renameNeedsSelect = false;
    bool m_showDeleteConfirm = false;
    bool m_deletePermanent = false;   // true when Shift held on delete
    char m_newFolderName[256] = {};
    char m_renameBuf[256] = {};
    FilePanel* m_contextPanel = nullptr;
    int m_contextIndex = -1;

    // File clipboard (copy/cut & paste)
    std::vector<std::string> m_clipboardPaths;   // full paths of copied/cut files
    bool m_clipboardIsAndroid = false;            // source is Android panel
    int m_clipboardDeviceSlot = 0;               // device slot of source (for Android)
    bool m_clipboardCut = false;                  // true = cut (move), false = copy
    void performClipboardPaste(FilePanel& dstPanel);

    float m_devicePollTimer = 0.0f;
    std::string m_statusMessage;
    std::chrono::steady_clock::time_point m_statusTime;
    bool m_firstFrame = true;

    // Background device polling thread
    std::thread m_pollThread;
    std::atomic<bool> m_shutdownPoll{false};
    std::atomic<bool> m_pollBusy{false};
    std::atomic<bool> m_tetheringInProgress{false}; // prevents batch thread from calling startServer during tethering
    std::chrono::steady_clock::time_point m_lastTransferActivity; // cooldown for health check
    std::mutex m_deviceMutex; // protects m_devices, m_selectedDevice
    void devicePollLoop();

    // Async action queue — UI thread posts, background worker executes
    // This ensures the UI NEVER blocks on DeviceClient operations
    std::thread m_asyncThread;
    std::mutex m_asyncMutex;
    std::condition_variable m_asyncCV;
    std::deque<std::function<void()>> m_asyncQueue;
    std::atomic<bool> m_asyncBusy{false};
    std::string m_asyncStatus; // shown in status bar while async action runs
    void asyncWorkerLoop();
    void postAsync(const std::string& statusMsg, std::function<void()> action);

    // Panel screen bounds (updated each frame for external drop targeting)
    ImVec2 m_leftPanelMin, m_leftPanelMax;
    ImVec2 m_rightPanelMin, m_rightPanelMax;

    FilePanel* m_lastFocusedPanel = nullptr; // tracks which panel the user last interacted with

    bool m_confirmStopTransfer = false;

    // Animation state
    float m_panelFadeLeft = 0.0f;   // 0..1, fades in on directory change
    float m_panelFadeRight = 0.0f;
    float m_overlayFade = 0.0f;     // transfer overlay fade
    std::chrono::steady_clock::time_point m_lastNavTimeLeft;
    std::chrono::steady_clock::time_point m_lastNavTimeRight;

    // Debug window
    bool m_showDebugWindow = false;
    bool m_debugAutoScroll = true;
    int m_debugLevelFilter = 0; // 0=All, 1=Info+, 2=Warn+, 3=Error only
    char m_debugTagFilter[64] = {};
    void renderDebugWindow();

};
