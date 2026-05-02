#include "app.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>
#include <tchar.h>
#include <algorithm>
#include <shobjidl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <fstream>
#include <string>

// System tray
#define WM_TRAYICON (WM_APP + 1)
#define IDM_TRAY_SHOW 1001
#define IDM_TRAY_EXIT 1002
static NOTIFYICONDATAW g_nid = {};
static bool g_trayIconAdded = false;
bool g_reallyQuit = false;

static void AddTrayIcon(HWND hwnd) {
    if (g_trayIconAdded) return;
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_INFO;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(101));
    wcscpy_s(g_nid.szTip, L"Fast Enough? - Android File Explorer");
    wcscpy_s(g_nid.szInfo, L"Fast Enough? - Android File Explorer is still running");
    wcscpy_s(g_nid.szInfoTitle, L"Fast Enough? - Android File Explorer");
    g_nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_trayIconAdded = true;
}

static void RemoveTrayIcon() {
    if (!g_trayIconAdded) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_trayIconAdded = false;
}

static void ShowTrayContextMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_SHOW, L"Show");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT, L"Exit");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(hMenu);
}

// Window state persistence
static std::string getSettingsPath() {
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, path))) {
        std::string dir = std::string(path) + "\\FastEnough";
        CreateDirectoryA(dir.c_str(), nullptr);
        return dir + "\\window.cfg";
    }
    return "";
}

static void saveWindowState(HWND hwnd) {
    WINDOWPLACEMENT wp{}; wp.length = sizeof(wp);
    GetWindowPlacement(hwnd, &wp);
    // Don't save if window is hidden (minimized to tray)
    if (!IsWindowVisible(hwnd)) return;
    int w = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
    int h = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
    if (w < 200 || h < 200) return;
    std::string path = getSettingsPath();
    if (path.empty()) return;
    std::ofstream f(path, std::ios::binary);
    if (f) f.write((const char*)&wp, sizeof(wp));
}

static bool loadWindowState(HWND hwnd) {
    std::string path = getSettingsPath();
    if (path.empty()) return false;
    std::ifstream f(path, std::ios::binary);
    WINDOWPLACEMENT wp{}; wp.length = sizeof(wp);
    if (f && f.read((char*)&wp, sizeof(wp))) {
        wp.length = sizeof(wp);
        // Validate: reject if saved while hidden/minimized with invalid dimensions
        int w = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
        int h = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
        if (w < 200 || h < 200) return false; // corrupted or saved while hidden
        // Don't restore hidden state — show normal or maximized
        if (wp.showCmd == SW_HIDE || wp.showCmd == SW_MINIMIZE || wp.showCmd == SW_SHOWMINIMIZED)
            wp.showCmd = SW_SHOWNORMAL;
        SetWindowPlacement(hwnd, &wp);
        return true;
    }
    return false;
}

// Taskbar progress (Windows 7+)
static ITaskbarList3* g_pTaskbar = nullptr;
HWND g_mainHwnd = nullptr;

void initTaskbarProgress(HWND hwnd) {
    g_mainHwnd = hwnd;
    OleInitialize(nullptr); // OLE drag-and-drop requires this (not just CoInitialize)
    CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&g_pTaskbar));
    if (g_pTaskbar) g_pTaskbar->HrInit();
}

void updateTaskbarProgress(float fraction, bool active) {
    if (!g_pTaskbar || !g_mainHwnd) return;
    if (!active) {
        g_pTaskbar->SetProgressState(g_mainHwnd, TBPF_NOPROGRESS);
    } else {
        g_pTaskbar->SetProgressState(g_mainHwnd, TBPF_NORMAL);
        g_pTaskbar->SetProgressValue(g_mainHwnd, (ULONGLONG)(fraction * 1000), 1000);
    }
}

void setTaskbarError() {
    if (!g_pTaskbar || !g_mainHwnd) return;
    g_pTaskbar->SetProgressState(g_mainHwnd, TBPF_ERROR);
}

void setTaskbarPaused() {
    if (!g_pTaskbar || !g_mainHwnd) return;
    g_pTaskbar->SetProgressState(g_mainHwnd, TBPF_PAUSED);
}

// Forward declarations
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();

// Live resize support — declared here so WinMain can access them
static App* g_pApp = nullptr;
static bool g_inSizeMove = false;
static void RenderFrame(); // defined after WndProc
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// DPI awareness
static float GetDpiScale(HWND hwnd) {
    // Try per-monitor DPI (Windows 10 1607+)
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32) {
        typedef UINT(WINAPI* GetDpiForWindowFn)(HWND);
        auto fn = (GetDpiForWindowFn)GetProcAddress(user32, "GetDpiForWindow");
        if (fn) {
            UINT dpi = fn(hwnd);
            if (dpi > 0) return (float)dpi / 96.0f;
        }
    }
    // Fallback: system DPI
    HDC hdc = GetDC(nullptr);
    float scale = (float)GetDeviceCaps(hdc, LOGPIXELSX) / 96.0f;
    ReleaseDC(nullptr, hdc);
    return scale;
}

static void EnableDpiAwareness() {
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32) {
        // Try SetProcessDpiAwarenessContext (Win10 1703+)
        typedef BOOL(WINAPI* SetProcessDpiAwarenessContextFn)(HANDLE);
        auto fn = (SetProcessDpiAwarenessContextFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (fn) {
            fn(/*DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2*/ (HANDLE)-4);
            return;
        }
    }
    HMODULE shcore = LoadLibraryA("shcore.dll");
    if (shcore) {
        typedef HRESULT(WINAPI* SetProcessDpiAwarenessFn)(int);
        auto fn = (SetProcessDpiAwarenessFn)GetProcAddress(shcore, "SetProcessDpiAwareness");
        if (fn) fn(2); // PROCESS_PER_MONITOR_DPI_AWARE
        FreeLibrary(shcore);
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    // Single instance — if already running, bring existing window to front
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"FastEnough_SingleInstance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Find and restore the existing window
        HWND existing = FindWindowW(L"FastEnoughClass", nullptr);
        if (existing) {
            ShowWindow(existing, SW_SHOW);
            SetForegroundWindow(existing);
        }
        CloseHandle(hMutex);
        return 0;
    }

    // Enable DPI awareness BEFORE creating any windows
    EnableDpiAwareness();

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"FastEnoughClass";
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(101));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    // Create window
    HWND hwnd = CreateWindowW(
        wc.lpszClassName, L"Fast Enough? - Android File Explorer",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1400, 800,
        nullptr, nullptr, hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, hInstance);
        return 1;
    }

    // Restore saved window position/size, or show default
    if (!loadWindowState(hwnd))
        ShowWindow(hwnd, SW_SHOWDEFAULT);
    else
        ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    initTaskbarProgress(hwnd);
    DragAcceptFiles(hwnd, TRUE);

    // Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Move imgui.ini to %APPDATA%\FastEnough\ instead of exe directory
    static std::string imguiIniPath;
    {
        std::string settingsPath = getSettingsPath(); // returns .../FastEnough/window.cfg
        imguiIniPath = settingsPath.substr(0, settingsPath.rfind('\\')) + "\\imgui.ini";
    }
    io.IniFilename = imguiIniPath.c_str();

    // Match Windows DPI scaling (100%=1.0, 125%=1.25, 150%=1.5, 200%=2.0, etc.)
    float dpiScale = GetDpiScale(hwnd);

    // Font size scales with Windows DPI setting so it looks correct on any PC.
    // 16px at 100% scaling -> 24px at 150% -> 32px at 200%, etc.
    float fontSize = 16.0f * dpiScale;

    ImFontConfig fontCfg;
    fontCfg.OversampleH = 3;
    fontCfg.OversampleV = 3;
    fontCfg.PixelSnapH = true;
    // RasterizerDensity=1 since fontSize already includes the DPI scale.
    // The font is rasterized at the exact pixel size we need - no rescaling artifacts.
    fontCfg.RasterizerDensity = 1.0f;

    char winDir[MAX_PATH];
    GetWindowsDirectoryA(winDir, MAX_PATH);
    std::string segoeUIPath = std::string(winDir) + "\\Fonts\\segoeui.ttf";

    // Include Latin, Latin Extended, Greek, Cyrillic, and common symbols
    // so non-ASCII folder/file names display correctly.
    static const ImWchar glyphRanges[] = {
        0x0020, 0x00FF, // Basic Latin + Latin Supplement
        0x0100, 0x024F, // Latin Extended-A + B
        0x0370, 0x03FF, // Greek and Coptic
        0x0400, 0x04FF, // Cyrillic
        0x2000, 0x206F, // General Punctuation
        0x2100, 0x214F, // Letterlike Symbols
        0,
    };

    ImFont* mainFont = nullptr;
    if (GetFileAttributesA(segoeUIPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        mainFont = io.Fonts->AddFontFromFileTTF(segoeUIPath.c_str(), fontSize, &fontCfg, glyphRanges);
    }
    if (!mainFont) {
        fontCfg.SizePixels = fontSize;
        mainFont = io.Fonts->AddFontDefault(&fontCfg);
    }

    io.Fonts->Build();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // Create app — set DPI scale, then applyScale handles font + style sizing
    App app;
    app.m_systemDpiScale = dpiScale;
    app.applyScale(app.m_theme.userScale);
    g_pApp = &app;

    // Main loop
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done) break;

        // Render frame (handles resize internally)
        if (!g_inSizeMove) // skip if WndProc is already rendering during resize
            RenderFrame();
    }
    g_pApp = nullptr;

    // Save window state before exit
    saveWindowState(hwnd);

    // Cleanup
    if (g_pTaskbar) { g_pTaskbar->Release(); g_pTaskbar = nullptr; }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInstance);

    return 0;
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION,
        &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
            featureLevelArray, 2, D3D11_SDK_VERSION,
            &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (FAILED(hr)) return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain)       { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice)       { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

static void RenderFrame() {
    if (!g_pApp || !g_pSwapChain || !g_pd3dDeviceContext) return;

    // Handle pending resize
    if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
        CleanupRenderTarget();
        g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
        g_ResizeWidth = g_ResizeHeight = 0;
        CreateRenderTarget();
    }

    // Apply deferred scale change before starting the new frame
    if (g_pApp->m_pendingScale > 0) {
        g_pApp->applyScale(g_pApp->m_pendingScale);
        g_pApp->m_scalePreview = g_pApp->m_theme.userScale; // sync slider
        g_pApp->m_theme.save();
        g_pApp->m_pendingScale = -1.0f;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    g_pApp->render();
    ImGui::Render();

    const float clear_color[4] = { 0.00f, 0.00f, 0.00f, 1.00f };
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_pSwapChain->Present(1, 0);
}

#define RESIZE_TIMER_ID 1

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Drag-out to Explorer: when ImGui drag is active and mouse leaves client area,
    // release ImGui's capture and start OLE DoDragDrop
    if (msg == WM_MOUSEMOVE && g_pApp && g_pApp->m_isDragging) {
        POINT screenPt;
        GetCursorPos(&screenPt);
        RECT clientRect;
        GetClientRect(hWnd, &clientRect);
        POINT clientPt = screenPt;
        ScreenToClient(hWnd, &clientPt);
        if (!PtInRect(&clientRect, clientPt)) {
            g_pApp->m_isDragging = false;
            FilePanel* src = g_pApp->m_dragSourcePanel;
            if (src && !src->isAndroid) {
                // Windows panel: use local file paths directly
                std::vector<std::string> paths;
                for (int idx : src->selectedIndices) {
                    if (!src->validIndex(idx)) continue;
                    std::string fp = src->currentPath;
                    if (fp.back() != '\\') fp += "\\";
                    fp += src->entryName(idx);
                    paths.push_back(fp);
                }
                if (!paths.empty()) {
                    ReleaseCapture();
                    g_pApp->performOleDragDrop(paths);
                    return 0;
                }
            } else if (src && src->isAndroid) {
                // Android panel: pull files to temp, then OLE drag-drop
                ReleaseCapture();
                g_pApp->performAndroidDragOut(src);
                return 0;
            }
        }
    }

    // Handle file drops from Explorer BEFORE ImGui (which might swallow the message)
    if (msg == WM_DROPFILES && g_pApp) {
        HDROP hDrop = (HDROP)wParam;
        UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::string> paths;
        for (UINT i = 0; i < count; i++) {
            WCHAR wpath[MAX_PATH];
            if (DragQueryFileW(hDrop, i, wpath, MAX_PATH)) {
                int len = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, nullptr, 0, nullptr, nullptr);
                std::string utf8(len - 1, '\0');
                WideCharToMultiByte(CP_UTF8, 0, wpath, -1, utf8.data(), len, nullptr, nullptr);
                paths.push_back(std::move(utf8));
            }
        }
        POINT pt;
        DragQueryPoint(hDrop, &pt);
        DragFinish(hDrop);
        g_pApp->handleExternalFileDrop(paths, pt.x, pt.y);
        return 0;
    }

    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        if (g_inSizeMove) RenderFrame(); // live resize
        return 0;
    case WM_ENTERSIZEMOVE:
        g_inSizeMove = true;
        SetTimer(hWnd, RESIZE_TIMER_ID, 16, nullptr); // ~60fps timer during resize
        return 0;
    case WM_EXITSIZEMOVE:
        g_inSizeMove = false;
        KillTimer(hWnd, RESIZE_TIMER_ID);
        return 0;
    case WM_TIMER:
        if (wParam == RESIZE_TIMER_ID) RenderFrame();
        return 0;
    case WM_CLOSE:
        if (!g_reallyQuit) {
            if (g_pApp && g_pApp->m_prefs.confirmOnClose) {
                // Show ImGui close confirmation dialog
                g_pApp->m_showCloseConfirm = true;
                return 0;
            } else {
                // Minimize to system tray instead of closing
                ShowWindow(hWnd, SW_HIDE);
                AddTrayIcon(hWnd);
                return 0;
            }
        }
        break; // fall through to DefWindowProc which calls DestroyWindow
    case WM_TRAYICON:
        if (lParam == WM_LBUTTONDBLCLK) {
            // Double-click tray icon — restore window
            ShowWindow(hWnd, SW_SHOW);
            SetForegroundWindow(hWnd);
            RemoveTrayIcon();
        } else if (lParam == WM_RBUTTONUP) {
            ShowTrayContextMenu(hWnd);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDM_TRAY_SHOW) {
            ShowWindow(hWnd, SW_SHOW);
            SetForegroundWindow(hWnd);
            RemoveTrayIcon();
        } else if (LOWORD(wParam) == IDM_TRAY_EXIT) {
            g_reallyQuit = true;
            RemoveTrayIcon();
            PostMessage(hWnd, WM_CLOSE, 0, 0);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}
