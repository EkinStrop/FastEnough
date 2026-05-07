#include "app.h"
#include "mcraw_local.h"
#include "mcraw_projfs.h"
#include "device_dokan.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <cctype>
#include <fstream>
#include <set>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <deque>
#include <ole2.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdi32.lib")

// Safe UTF-8 path-to-string helper (avoids ANSI code page issues with .string())
static std::string pathToUtf8(const std::filesystem::path& p) {
    auto ws = p.wstring();
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), utf8.data(), len, nullptr, nullptr);
    return utf8;
}

// UTF-8 to wide string helper for Unicode-safe Win32 API calls
static std::wstring toWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    std::wstring wide(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), wide.data(), len);
    return wide;
}

// UTF-8 string to std::filesystem::path (via wide string to avoid ANSI codepage issues)
static std::filesystem::path toFsPath(const std::string& utf8) {
    return std::filesystem::path(toWide(utf8));
}

static std::vector<DetectedNic> enumerateNics() {
    std::vector<DetectedNic> result;
    ULONG bufLen = 16384;
    std::vector<BYTE> buf(bufLen);
    ULONG flags = GAA_FLAG_INCLUDE_PREFIX;
    DWORD ret = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                      (PIP_ADAPTER_ADDRESSES)buf.data(), &bufLen);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buf.resize(bufLen);
        ret = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                    (PIP_ADAPTER_ADDRESSES)buf.data(), &bufLen);
    }
    if (ret != NO_ERROR) return result;

    for (auto* adapter = (PIP_ADAPTER_ADDRESSES)buf.data(); adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) continue;
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (adapter->IfType != IF_TYPE_ETHERNET_CSMACD &&
            adapter->IfType != IF_TYPE_IEEE80211) continue;

        for (auto* ua = adapter->FirstUnicastAddress; ua; ua = ua->Next) {
            if (ua->Address.lpSockaddr->sa_family != AF_INET) continue;
            sockaddr_in* sa = (sockaddr_in*)ua->Address.lpSockaddr;
            char ipStr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &sa->sin_addr, ipStr, sizeof(ipStr));
            std::string ip(ipStr);
            if (ip == "127.0.0.1") continue;

            DetectedNic nic;
            int wlen = (int)wcslen(adapter->FriendlyName);
            int mbLen = WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, wlen, nullptr, 0, nullptr, nullptr);
            nic.adapterName.resize(mbLen);
            WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, wlen, nic.adapterName.data(), mbLen, nullptr, nullptr);
            nic.ipAddress = ip;
            nic.speed = adapter->TransmitLinkSpeed;
            wlen = (int)wcslen(adapter->Description);
            mbLen = WideCharToMultiByte(CP_UTF8, 0, adapter->Description, wlen, nullptr, 0, nullptr, nullptr);
            nic.description.resize(mbLen);
            WideCharToMultiByte(CP_UTF8, 0, adapter->Description, wlen, nic.description.data(), mbLen, nullptr, nullptr);
            result.push_back(std::move(nic));
        }
    }
    return result;
}

class SimpleDropSource : public IDropSource {
    LONG m_ref = 1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropSource) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = InterlockedDecrement(&m_ref); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL fEscapePressed, DWORD grfKeyState) override {
        if (fEscapePressed) return DRAGDROP_S_CANCEL;
        if (!(grfKeyState & MK_LBUTTON)) return DRAGDROP_S_DROP;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};

class SimpleDataObject : public IDataObject {
    LONG m_ref = 1;
    struct Entry { FORMATETC fmt; STGMEDIUM stg; };
    std::vector<Entry> m_entries;

    static HGLOBAL dupGlobal(HGLOBAL src) {
        if (!src) return nullptr;
        SIZE_T sz = GlobalSize(src);
        HGLOBAL dst = GlobalAlloc(GHND, sz);
        if (!dst) return nullptr;
        void* s = GlobalLock(src); void* d = GlobalLock(dst);
        memcpy(d, s, sz);
        GlobalUnlock(dst); GlobalUnlock(src);
        return dst;
    }
    static bool fmtMatch(const FORMATETC& a, const FORMATETC& b) {
        return a.cfFormat == b.cfFormat && a.dwAspect == b.dwAspect && a.lindex == b.lindex;
    }
public:
    ~SimpleDataObject() {
        for (auto& e : m_entries) ReleaseStgMedium(&e.stg);
    }

    void setHDrop(const std::vector<std::wstring>& files) {
        size_t totalChars = 0;
        for (auto& f : files) totalChars += f.size() + 1;
        totalChars += 1;
        size_t bufSize = sizeof(DROPFILES) + totalChars * sizeof(wchar_t);

        HGLOBAL hGlobal = GlobalAlloc(GHND, bufSize);
        if (!hGlobal) return;
        auto* df = (DROPFILES*)GlobalLock(hGlobal);
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        wchar_t* dst = (wchar_t*)((char*)df + sizeof(DROPFILES));
        for (auto& f : files) {
            memcpy(dst, f.c_str(), (f.size() + 1) * sizeof(wchar_t));
            dst += f.size() + 1;
        }
        *dst = L'\0';
        GlobalUnlock(hGlobal);

        Entry e{};
        e.fmt.cfFormat = CF_HDROP;
        e.fmt.dwAspect = DVASPECT_CONTENT;
        e.fmt.lindex = -1;
        e.fmt.tymed = TYMED_HGLOBAL;
        e.stg.tymed = TYMED_HGLOBAL;
        e.stg.hGlobal = hGlobal;
        m_entries.push_back(e);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDataObject) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = InterlockedDecrement(&m_ref); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* fmt, STGMEDIUM* stg) override {
        for (auto& e : m_entries) {
            if (fmtMatch(e.fmt, *fmt) && (e.fmt.tymed & fmt->tymed)) {
                stg->tymed = e.stg.tymed;
                stg->pUnkForRelease = nullptr;
                if (e.stg.tymed == TYMED_HGLOBAL) {
                    stg->hGlobal = dupGlobal(e.stg.hGlobal);
                    if (!stg->hGlobal) return E_OUTOFMEMORY;
                    return S_OK;
                } else if (e.stg.tymed == TYMED_ISTREAM) {
                    stg->pstm = e.stg.pstm;
                    if (stg->pstm) stg->pstm->AddRef();
                    return S_OK;
                } else if (e.stg.tymed == TYMED_GDI) {
                    stg->hBitmap = e.stg.hBitmap;
                    return S_OK;
                }
                return DV_E_TYMED;
            }
        }
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* fmt) override {
        for (auto& e : m_entries) {
            if (fmtMatch(e.fmt, *fmt) && (e.fmt.tymed & fmt->tymed)) return S_OK;
        }
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        out->ptd = nullptr; return DATA_S_SAMEFORMATETC;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* fmt, STGMEDIUM* stg, BOOL fRelease) override {
        if (!fmt || !stg) return E_INVALIDARG;
        // Replace existing entry of same format
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            if (fmtMatch(it->fmt, *fmt)) {
                ReleaseStgMedium(&it->stg);
                m_entries.erase(it);
                break;
            }
        }
        Entry e{};
        e.fmt = *fmt;
        if (fRelease) {
            e.stg = *stg;
        } else {
            // Copy the medium
            e.stg.tymed = stg->tymed;
            e.stg.pUnkForRelease = nullptr;
            if (stg->tymed == TYMED_HGLOBAL) {
                e.stg.hGlobal = dupGlobal(stg->hGlobal);
                if (!e.stg.hGlobal) return E_OUTOFMEMORY;
            } else if (stg->tymed == TYMED_ISTREAM) {
                e.stg.pstm = stg->pstm;
                if (e.stg.pstm) e.stg.pstm->AddRef();
            } else {
                e.stg = *stg;
            }
        }
        m_entries.push_back(e);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dir, IEnumFORMATETC** ppEnum) override {
        if (dir != DATADIR_GET) { *ppEnum = nullptr; return E_NOTIMPL; }
        std::vector<FORMATETC> fmts;
        for (auto& e : m_entries) fmts.push_back(e.fmt);
        return SHCreateStdEnumFmtEtc((UINT)fmts.size(), fmts.empty() ? nullptr : fmts.data(), ppEnum);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
};

// Shared state for AdbPullStream — held via shared_ptr so the producer thread
// can outlive the IStream object on cancel without us blocking IStream::Release.
struct AdbStreamState {
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<std::vector<char>> queue;
    size_t queueBytes = 0;
    size_t frontConsumed = 0;
    uint64_t pos = 0;
    bool eof = false;
    bool aborted = false;
};

// IStream that streams a remote file via a producer thread running
// DeviceClient::readRangeStreaming. State is shared with the producer so
// destruction is non-blocking — abort flag is set, the producer is detached,
// and it cleans up when the device finishes draining.
class AdbPullStream : public IStream {
    LONG m_ref = 1;
    std::string m_remotePath;
    DeviceClient* m_dev = nullptr;
    uint64_t m_size = 0;
    std::wstring m_name;
    std::shared_ptr<AdbStreamState> m_state;
    bool m_producerStarted = false;
    std::atomic<int>* m_inflightCounter = nullptr;

    static constexpr size_t kMaxQueue = 128 * 1024 * 1024; // 128 MiB read-ahead cap

    // Caller holds m_state->mtx. Aborts any prior producer (detached) and starts
    // a new one at startOffset. The new producer captures a fresh shared_ptr to
    // its own state — restarting effectively replaces m_state.
    void restartProducerLocked(std::unique_lock<std::mutex>& lk, uint64_t startOffset) {
        // Signal the current producer to abort and detach it; it will finish
        // draining on its own and then drop its shared_ptr<State>.
        m_state->aborted = true;
        m_state->cv.notify_all();
        lk.unlock();

        // New, independent state for the new producer / consumer pair.
        auto fresh = std::make_shared<AdbStreamState>();
        fresh->pos = startOffset;
        m_state = fresh;
        lk = std::unique_lock<std::mutex>(m_state->mtx);
        m_producerStarted = true;
        if (startOffset >= m_size) { m_state->eof = true; return; }

        std::shared_ptr<AdbStreamState> stateRef = m_state;
        DeviceClient* dev = m_dev;
        std::string remote = m_remotePath;
        uint64_t length = m_size - startOffset;
        std::atomic<int>* inflight = m_inflightCounter;
        std::thread([stateRef, dev, remote, startOffset, length, inflight]() {
            if (inflight) inflight->fetch_add(1);
            dev->readRangeStreaming(remote, startOffset, length,
                [stateRef](const void* data, uint32_t len, uint64_t /*offset*/) -> bool {
                    // Apply backpressure first (mutex briefly held).
                    {
                        std::unique_lock<std::mutex> lk(stateRef->mtx);
                        stateRef->cv.wait(lk, [&] {
                            return stateRef->aborted || stateRef->queueBytes < kMaxQueue;
                        });
                        if (stateRef->aborted) return false;
                    }
                    // Copy out of the device's reusable buffer outside the lock so
                    // the consumer is not blocked during the per-chunk memcpy.
                    std::vector<char> chunk((const char*)data, (const char*)data + len);
                    {
                        std::lock_guard<std::mutex> lk(stateRef->mtx);
                        if (stateRef->aborted) return false;
                        stateRef->queue.push_back(std::move(chunk));
                        stateRef->queueBytes += len;
                        stateRef->cv.notify_all();
                    }
                    return true;
                });
            {
                std::lock_guard<std::mutex> lk(stateRef->mtx);
                stateRef->eof = true;
                stateRef->cv.notify_all();
            }
            if (inflight) inflight->fetch_sub(1);
        }).detach();
    }

public:
    AdbPullStream(std::string remotePath, DeviceClient* dev, uint64_t size, std::wstring name,
                  std::atomic<int>* inflight)
        : m_remotePath(std::move(remotePath)), m_dev(dev), m_size(size), m_name(std::move(name)),
          m_state(std::make_shared<AdbStreamState>()), m_inflightCounter(inflight) {}
    ~AdbPullStream() {
        // Tell the (possibly running) producer to abort; do NOT join — we want
        // IStream::Release to return immediately so Explorer's cancel is fast.
        std::lock_guard<std::mutex> lk(m_state->mtx);
        m_state->aborted = true;
        m_state->cv.notify_all();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_ISequentialStream || riid == IID_IStream) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Read(void* pv, ULONG cb, ULONG* pcbRead) override {
        if (pcbRead) *pcbRead = 0;
        std::unique_lock<std::mutex> lk(m_state->mtx);
        if (!m_producerStarted) restartProducerLocked(lk, m_state->pos);
        auto* st = m_state.get();
        if (st->pos >= m_size) return S_FALSE;
        ULONG written = 0;
        while (written < cb && st->pos < m_size) {
            if (st->queue.empty()) {
                if (st->eof) break;
                st->cv.wait(lk, [&] { return !st->queue.empty() || st->eof || st->aborted; });
                if (st->aborted) return E_FAIL;
                continue;
            }
            auto& front = st->queue.front();
            size_t avail = front.size() - st->frontConsumed;
            size_t want = cb - written;
            size_t take = (avail < want) ? avail : want;
            memcpy((char*)pv + written, front.data() + st->frontConsumed, take);
            written += (ULONG)take;
            st->frontConsumed += take;
            st->pos += take;
            st->queueBytes -= take;
            if (st->frontConsumed == front.size()) {
                st->queue.pop_front();
                st->frontConsumed = 0;
            }
            st->cv.notify_all();
        }
        if (pcbRead) *pcbRead = written;
        if (written == 0) return S_FALSE;
        return (written < cb) ? S_FALSE : S_OK;
    }
    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER off, DWORD origin, ULARGE_INTEGER* newPos) override {
        std::unique_lock<std::mutex> lk(m_state->mtx);
        auto* st = m_state.get();
        int64_t base = 0;
        if (origin == STREAM_SEEK_SET) base = 0;
        else if (origin == STREAM_SEEK_CUR) base = (int64_t)st->pos;
        else if (origin == STREAM_SEEK_END) base = (int64_t)m_size;
        else return STG_E_INVALIDFUNCTION;
        int64_t np = base + off.QuadPart;
        if (np < 0) return STG_E_INVALIDFUNCTION;
        uint64_t want = (uint64_t)np;
        if (want == st->pos) {
            if (newPos) newPos->QuadPart = st->pos;
            return S_OK;
        }
        if (want > st->pos && (want - st->pos) <= st->queueBytes) {
            uint64_t skip = want - st->pos;
            while (skip > 0 && !st->queue.empty()) {
                auto& front = st->queue.front();
                size_t avail = front.size() - st->frontConsumed;
                size_t take = (size_t)((skip < avail) ? skip : avail);
                st->frontConsumed += take;
                st->queueBytes -= take;
                skip -= take;
                st->pos += take;
                if (st->frontConsumed == front.size()) {
                    st->queue.pop_front();
                    st->frontConsumed = 0;
                }
            }
            st->cv.notify_all();
            if (newPos) newPos->QuadPart = st->pos;
            return S_OK;
        }
        // Restart producer at new offset (replaces m_state).
        restartProducerLocked(lk, want);
        if (newPos) newPos->QuadPart = m_state->pos;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream* dst, ULARGE_INTEGER cb,
                                     ULARGE_INTEGER* read, ULARGE_INTEGER* written) override {
        ULARGE_INTEGER tot{}; ULARGE_INTEGER w{};
        std::vector<char> buf(1 << 20); // 1 MiB
        while (cb.QuadPart > 0) {
            ULONG to = (ULONG)((cb.QuadPart < buf.size()) ? cb.QuadPart : buf.size());
            ULONG got = 0;
            HRESULT hr = Read(buf.data(), to, &got);
            if (FAILED(hr)) return hr;
            if (got == 0) break;
            tot.QuadPart += got;
            ULONG ww = 0;
            if (dst) dst->Write(buf.data(), got, &ww);
            w.QuadPart += ww;
            cb.QuadPart -= got;
            if (got < to) break;
        }
        if (read) *read = tot;
        if (written) *written = w;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* p, DWORD flags) override {
        if (!p) return E_POINTER;
        memset(p, 0, sizeof(*p));
        if (!(flags & STATFLAG_NONAME)) {
            size_t nb = (m_name.size() + 1) * sizeof(wchar_t);
            p->pwcsName = (LPOLESTR)CoTaskMemAlloc(nb);
            if (p->pwcsName) memcpy(p->pwcsName, m_name.c_str(), nb);
        }
        p->type = STGTY_STREAM;
        p->cbSize.QuadPart = m_size;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }
};

// Register virtual files (FILEGROUPDESCRIPTORW + per-index FILECONTENTS streams)
// on the data object. Streams ownership is transferred (refcount stays at the
// caller's, we AddRef when we store).
static void setVirtualFilesOnDataObject(SimpleDataObject* obj,
                                        const std::vector<std::wstring>& names,
                                        const std::vector<uint64_t>& sizes,
                                        const std::vector<IStream*>& streams) {
    UINT cfDesc = RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
    UINT cfCont = RegisterClipboardFormatW(CFSTR_FILECONTENTS);

    // Build FILEGROUPDESCRIPTORW
    size_t n = names.size();
    size_t bufSize = sizeof(FILEGROUPDESCRIPTORW) + (n - 1) * sizeof(FILEDESCRIPTORW);
    HGLOBAL hg = GlobalAlloc(GHND, bufSize);
    if (hg) {
        auto* fgd = (FILEGROUPDESCRIPTORW*)GlobalLock(hg);
        fgd->cItems = (UINT)n;
        for (size_t i = 0; i < n; i++) {
            FILEDESCRIPTORW& fd = fgd->fgd[i];
            fd.dwFlags = FD_FILESIZE | FD_PROGRESSUI;
            fd.nFileSizeLow = (DWORD)(sizes[i] & 0xFFFFFFFF);
            fd.nFileSizeHigh = (DWORD)(sizes[i] >> 32);
            const std::wstring& nm = names[i];
            size_t copy = nm.size() < (MAX_PATH - 1) ? nm.size() : (MAX_PATH - 1);
            memcpy(fd.cFileName, nm.c_str(), copy * sizeof(wchar_t));
            fd.cFileName[copy] = L'\0';
        }
        GlobalUnlock(hg);

        FORMATETC fmt{};
        fmt.cfFormat = (CLIPFORMAT)cfDesc;
        fmt.dwAspect = DVASPECT_CONTENT;
        fmt.lindex = -1;
        fmt.tymed = TYMED_HGLOBAL;
        STGMEDIUM stg{};
        stg.tymed = TYMED_HGLOBAL;
        stg.hGlobal = hg;
        obj->SetData(&fmt, &stg, TRUE);
    }

    // Per-index FILECONTENTS
    for (size_t i = 0; i < n; i++) {
        FORMATETC fmt{};
        fmt.cfFormat = (CLIPFORMAT)cfCont;
        fmt.dwAspect = DVASPECT_CONTENT;
        fmt.lindex = (LONG)i;
        fmt.tymed = TYMED_ISTREAM;
        STGMEDIUM stg{};
        stg.tymed = TYMED_ISTREAM;
        stg.pstm = streams[i];
        if (stg.pstm) stg.pstm->AddRef();
        obj->SetData(&fmt, &stg, TRUE);
    }
}

// Build a small RGB bitmap with drag label text. Caller owns the returned HBITMAP.
static HBITMAP createDragLabelBitmap(const std::wstring& label, COLORREF& outColorKey) {
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);

    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    LOGFONTW lf{};
    GetObjectW(font, sizeof(lf), &lf);
    lf.lfHeight = -16;
    HFONT bigFont = CreateFontIndirectW(&lf);

    HFONT oldFont = (HFONT)SelectObject(mem, bigFont);
    RECT measure{0,0,0,0};
    DrawTextW(mem, label.c_str(), -1, &measure, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    int w = measure.right + 16;
    int h = measure.bottom + 8;
    if (w < 64) w = 64;
    if (h < 24) h = 24;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP oldBmp = (HBITMAP)SelectObject(mem, bmp);

    // Magenta color key background
    outColorKey = RGB(255, 0, 255);
    HBRUSH bgBrush = CreateSolidBrush(outColorKey);
    RECT full{0,0,w,h};
    FillRect(mem, &full, bgBrush);
    DeleteObject(bgBrush);

    // Rounded panel
    HBRUSH panelBrush = CreateSolidBrush(RGB(30, 50, 80));
    HPEN panelPen = CreatePen(PS_SOLID, 1, RGB(80, 130, 200));
    HBRUSH oldBrush = (HBRUSH)SelectObject(mem, panelBrush);
    HPEN oldPen = (HPEN)SelectObject(mem, panelPen);
    RoundRect(mem, 0, 0, w, h, 8, 8);
    SelectObject(mem, oldBrush);
    SelectObject(mem, oldPen);
    DeleteObject(panelBrush);
    DeleteObject(panelPen);

    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(220, 235, 255));
    RECT textRect{8, 4, w - 8, h - 4};
    DrawTextW(mem, label.c_str(), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    SelectObject(mem, oldFont);
    SelectObject(mem, oldBmp);
    DeleteObject(bigFont);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return bmp;
}

// Attach a label drag image to the data object via IDragSourceHelper2.
static void attachDragImage(IDataObject* dataObj, const std::wstring& label) {
    IDragSourceHelper2* dsh2 = nullptr;
    if (FAILED(CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dsh2)))) return;
    SHDRAGIMAGE img{};
    COLORREF key = 0;
    img.hbmpDragImage = createDragLabelBitmap(label, key);
    if (img.hbmpDragImage) {
        BITMAP bm{};
        GetObject(img.hbmpDragImage, sizeof(bm), &bm);
        img.sizeDragImage.cx = bm.bmWidth;
        img.sizeDragImage.cy = bm.bmHeight;
        img.ptOffset.x = bm.bmWidth / 2;
        img.ptOffset.y = bm.bmHeight / 2;
        img.crColorKey = key;
        if (FAILED(dsh2->InitializeFromBitmap(&img, dataObj))) {
            DeleteObject(img.hbmpDragImage);
        }
    }
    dsh2->Release();
}

static std::wstring buildDragLabel(const std::vector<std::wstring>& items) {
    if (items.size() == 1) {
        const std::wstring& p = items[0];
        size_t slash = p.find_last_of(L"\\/");
        return (slash == std::wstring::npos) ? p : p.substr(slash + 1);
    }
    return std::to_wstring(items.size()) + L" files";
}

void App::performOleDragDrop(const std::vector<std::string>& filePaths) {
    std::vector<std::wstring> wPaths;
    for (auto& p : filePaths) {
        int len = MultiByteToWideChar(CP_UTF8, 0, p.c_str(), -1, nullptr, 0);
        std::wstring wp(len - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, p.c_str(), -1, wp.data(), len);
        wPaths.push_back(std::move(wp));
    }
    auto* dataObj = new SimpleDataObject();
    dataObj->setHDrop(wPaths);
    auto* dropSrc = new SimpleDropSource();

    attachDragImage(dataObj, buildDragLabel(wPaths));

    DWORD effect = 0;
    DoDragDrop(dataObj, dropSrc, DROPEFFECT_COPY | DROPEFFECT_MOVE, &effect);

    // If Explorer performed a move, delete the source files
    if (effect == DROPEFFECT_MOVE) {
        for (auto& p : filePaths) {
            try { std::filesystem::remove(toFsPath(p)); } catch (...) {}
        }
    }

    // Clear selection on the source panel so it doesn't look stuck after the drop.
    if (m_dragSourcePanel) {
        m_dragSourcePanel->selectedIndices.clear();
        m_dragSourcePanel->focusedIndex = -1;
    }
    m_dragSourcePanel = nullptr;

    // Refresh panels after any drag-out (file may have been moved/copied)
    m_leftPanel.needsRefresh = true;
    m_rightPanel.needsRefresh = true;

    dropSrc->Release();
    dataObj->Release();
}

void App::performAndroidDragOut(FilePanel* src) {
    if (!src || !src->isAndroid) return;
    DeviceClient* dev = &deviceFor(*src);
    if (!dev->isServerRunning()) return;

    std::vector<std::string> remotePaths;
    std::vector<std::wstring> wNames;
    std::vector<uint64_t> sizes;
    for (int idx : src->selectedIndices) {
        if (!src->validIndex(idx)) continue;
        if (src->entryIsDir(idx)) continue;
        std::string name = src->entryName(idx);
        std::string remotePath = src->currentPath;
        if (remotePath.back() != '/') remotePath += "/";
        remotePath += name;
        remotePaths.push_back(remotePath);
        wNames.push_back(toWide(name));
        sizes.push_back(src->entrySize(idx));
    }
    if (remotePaths.empty()) return;

    std::string serial = dev->connectedSerial();
    FilePanel* srcPanel = src;
    DWORD uiThreadId = GetCurrentThreadId();

    // Run DoDragDrop on a dedicated STA thread so Explorer's IStream::Read calls
    // (which may pull large files via ADB) don't block the UI thread.
    std::thread worker([this, dev, serial, remotePaths, wNames, sizes, srcPanel, uiThreadId]() {
        OleInitialize(nullptr);
        // Share input state with the UI thread so DoDragDrop sees the live mouse
        // button state and can render the drag image as the user moves the cursor.
        AttachThreadInput(GetCurrentThreadId(), uiThreadId, TRUE);

        auto* dataObj = new SimpleDataObject();
        auto* dropSrc = new SimpleDropSource();

        std::vector<IStream*> streams;
        streams.reserve(remotePaths.size());
        for (size_t i = 0; i < remotePaths.size(); i++) {
            streams.push_back(new AdbPullStream(remotePaths[i], dev, sizes[i], wNames[i],
                                                &m_androidDragInflight));
        }
        setVirtualFilesOnDataObject(dataObj, wNames, sizes, streams);
        for (auto* s : streams) s->Release();

        attachDragImage(dataObj, buildDragLabel(wNames));

        DWORD effect = 0;
        DoDragDrop(dataObj, dropSrc, DROPEFFECT_COPY | DROPEFFECT_MOVE, &effect);

        dropSrc->Release();
        dataObj->Release();

        if (srcPanel) {
            srcPanel->selectedIndices.clear();
            srcPanel->focusedIndex = -1;
        }
        m_leftPanel.needsRefresh = true;
        m_rightPanel.needsRefresh = true;
        m_dragSourcePanel = nullptr;

        AttachThreadInput(GetCurrentThreadId(), uiThreadId, FALSE);
        OleUninitialize();
    });
    worker.detach();
}

// Check if a serial is a WiFi ADB connection (IP:port format like "192.168.x.x:5555")
static bool isWifiSerial(const std::string& serial) {
    auto colon = serial.rfind(':');
    if (colon == std::string::npos || colon == 0) return false;
    return serial.substr(0, colon).find('.') != std::string::npos;
}

static bool endsWithCI(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    return _stricmp(s.c_str() + s.size() - suffix.size(), suffix.c_str()) == 0;
}

static std::string getPrefsPath() {
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, path))) {
        std::string dir = std::string(path) + "\\FastEnough";
        CreateDirectoryA(dir.c_str(), nullptr);
        return dir + "\\preferences.ini";
    }
    return "";
}

void AppPreferences::save() {
    std::string path = getPrefsPath();
    if (path.empty()) return;
    std::ofstream f(path);
    if (!f) return;
    f << "[Preferences]\n";
    f << "restartAdbOnLaunch=" << (restartAdbOnLaunch ? 1 : 0) << "\n";
    f << "enableCrcVerification=" << (enableCrcVerification ? 1 : 0) << "\n";
    f << "autoDismissTransfer=" << (autoDismissTransfer ? 1 : 0) << "\n";
    f << "wifiAutoConnect=" << (wifiAutoConnect ? 1 : 0) << "\n";
    f << "confirmOnClose=" << (confirmOnClose ? 1 : 0) << "\n";
    f << "killAdbOnClose=" << (killAdbOnClose ? 1 : 0) << "\n";
    for (auto& w : savedWifiDevices) {
        f << "wifiDevice=" << w.serial << "|" << w.wifiIp << "|" << w.port << "|" << (w.autoConnect ? 1 : 0) << "|" << w.model << "\n";
    }
    f << "enableMultiNic=" << (enableMultiNic ? 1 : 0) << "\n";
    f << "usbPipeCount=" << usbPipeCount << "\n";
    f << "wifiPipeCount=" << wifiPipeCount << "\n";
    for (auto& nb : multiNicBindings) {
        f << "nicBinding=" << nb.adapterName << "|" << nb.localIp << "|" << (nb.enabled ? 1 : 0) << "\n";
    }
}

void AppPreferences::load() {
    std::string path = getPrefsPath();
    if (path.empty()) return;
    std::ifstream f(path);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if (line.find("restartAdbOnLaunch=") == 0)
            restartAdbOnLaunch = (line.back() == '1');
        else if (line.find("enableCrcVerification=") == 0)
            enableCrcVerification = (line.back() == '1');
        else if (line.find("autoDismissTransfer=") == 0)
            autoDismissTransfer = (line.back() == '1');
        else if (line.find("wifiAutoConnect=") == 0)
            wifiAutoConnect = (line.back() == '1');
        else if (line.find("confirmOnClose=") == 0)
            confirmOnClose = (line.back() == '1');
        else if (line.find("killAdbOnClose=") == 0)
            killAdbOnClose = (line.back() == '1');
        else if (line.find("wifiDevice=") == 0) {
            std::string val = line.substr(11);
            SavedWifiDevice w;
            auto p1 = val.find('|'); if (p1 == std::string::npos) continue;
            auto p2 = val.find('|', p1+1); if (p2 == std::string::npos) continue;
            auto p3 = val.find('|', p2+1); if (p3 == std::string::npos) continue;
            w.serial = val.substr(0, p1);
            w.wifiIp = val.substr(p1+1, p2-p1-1);
            w.port = std::stoi(val.substr(p2+1, p3-p2-1));
            auto p4 = val.find('|', p3+1);
            if (p4 != std::string::npos) {
                w.autoConnect = (val[p3+1] == '1');
                w.model = val.substr(p4+1);
            } else {
                w.autoConnect = (val.back() == '1');
            }
            savedWifiDevices.push_back(std::move(w));
        }
        else if (line.find("enableMultiNic=") == 0)
            enableMultiNic = (line.back() == '1');
        else if (line.find("usbPipeCount=") == 0)
            usbPipeCount = std::clamp(std::stoi(line.substr(13)), 1, 2);
        else if (line.find("wifiPipeCount=") == 0)
            wifiPipeCount = std::clamp(std::stoi(line.substr(14)), 1, 2);
        else if (line.find("nicBinding=") == 0) {
            std::string val = line.substr(11);
            NicBinding nb;
            auto p1 = val.find('|'); if (p1 == std::string::npos) continue;
            auto p2 = val.find('|', p1+1); if (p2 == std::string::npos) continue;
            nb.adapterName = val.substr(0, p1);
            nb.localIp = val.substr(p1+1, p2-p1-1);
            nb.enabled = (val[p2+1] == '1');
            multiNicBindings.push_back(std::move(nb));
        }
    }
}

std::string AppTheme::getThemePath() {
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, path))) {
        std::string dir = std::string(path) + "\\FastEnough";
        CreateDirectoryA(dir.c_str(), nullptr);
        return dir + "\\theme.ini";
    }
    return "";
}

void AppTheme::save() {
    std::string path = getThemePath();
    if (path.empty()) return;
    std::ofstream f(path);
    if (!f) return;
    f << "[Theme]\n";
    f << "userScale=" << userScale << "\n";
    f << "customColors=" << (customColors ? 1 : 0) << "\n";
    if (customColors) {
        for (int i = 0; i < ImGuiCol_COUNT; i++) {
            f << "color" << i << "="
              << colors[i].x << "," << colors[i].y << ","
              << colors[i].z << "," << colors[i].w << "\n";
        }
    }
}

void AppTheme::load() {
    std::string path = getThemePath();
    if (path.empty()) return;
    std::ifstream f(path);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if (line.find("userScale=") == 0) {
            userScale = std::stof(line.substr(10));
            if (userScale < 0.5f) userScale = 0.5f;
            if (userScale > 3.0f) userScale = 3.0f;
        } else if (line.find("customColors=") == 0) {
            customColors = (line.back() == '1');
        } else if (line.find("color") == 0) {
            // Parse "colorN=r,g,b,a"
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            int idx = std::stoi(line.substr(5, eq - 5));
            if (idx < 0 || idx >= ImGuiCol_COUNT) continue;
            std::string vals = line.substr(eq + 1);
            float r, g, b, a;
            if (sscanf(vals.c_str(), "%f,%f,%f,%f", &r, &g, &b, &a) == 4) {
                colors[idx] = ImVec4(r, g, b, a);
            }
        }
    }
}

// Defined in main.cpp — for real quit (bypassing minimize-to-tray)
extern bool g_reallyQuit;
extern HWND g_mainHwnd;

// IFileOperation-based delete: recycle bin or permanent
static bool deleteWindowsFiles(const std::vector<std::string>& paths, bool permanent, std::string& error) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool needUninit = SUCCEEDED(hr);

    IFileOperation* pfo = nullptr;
    hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pfo));
    if (FAILED(hr)) {
        error = "Failed to create IFileOperation";
        if (needUninit) CoUninitialize();
        return false;
    }

    DWORD flags = FOF_NOCONFIRMATION | FOF_SILENT;
    if (!permanent) flags |= FOFX_RECYCLEONDELETE;
    // FOF_ALLOWUNDO also enables recycle for older behavior
    if (!permanent) flags |= FOF_ALLOWUNDO;

    pfo->SetOperationFlags(flags);

    for (const auto& path : paths) {
        // Convert to wide string
        int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        std::wstring wpath(wlen, 0);
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);

        IShellItem* pItem = nullptr;
        hr = SHCreateItemFromParsingName(wpath.c_str(), nullptr, IID_PPV_ARGS(&pItem));
        if (SUCCEEDED(hr)) {
            pfo->DeleteItem(pItem, nullptr);
            pItem->Release();
        }
    }

    hr = pfo->PerformOperations();
    pfo->Release();
    if (needUninit) CoUninitialize();

    if (FAILED(hr)) {
        error = "Delete operation failed (HRESULT: " + std::to_string(hr) + ")";
        return false;
    }
    return true;
}

App::App() {
    m_prefs.load();
    m_theme.load();

    // Second device uses a different local port for ADB forward
    m_deviceSlots[1].setLocalPort(AFM_PORT + 1);
    // Secondary channel (dual-channel WiFi) uses a different port to avoid ADB forward collision
    m_secondaryChannel.setLocalPort(AFM_PORT + 2);

    m_leftPanel.isAndroid = false;
    m_leftPanel.currentPath = "C:\\";
    strcpy_s(m_leftPanel.pathInput, m_leftPanel.currentPath.c_str());

    m_rightPanel.isAndroid = true;
    m_rightPanel.currentPath = "/";  // will be updated once device is detected
    strcpy_s(m_rightPanel.pathInput, m_rightPanel.currentPath.c_str());

    m_shutdownTransfer = false;
    m_batchThread = std::thread([this]() { processBatchQueue(); });

    m_shutdownPoll = false;
    m_pollThread = std::thread([this]() { devicePollLoop(); });

    m_asyncThread = std::thread([this]() { asyncWorkerLoop(); });

    // setupStyle sets base sizes + colors, then we scale for DPI + user preference
    // m_systemDpiScale is set by main.cpp AFTER construction, so initial scaling
    // is handled by applyScale() called from the first render frame.
    // For now, set base style — main.cpp will set m_systemDpiScale and the first
    // render handles the rest.
    setupStyle();
}

App::~App() {
    m_shutdownTransfer = true;
    m_shutdownPoll = true;
    m_batchCV.notify_all();
    m_asyncCV.notify_all();
    if (m_batchThread.joinable()) m_batchThread.join();
    if (m_pollThread.joinable()) m_pollThread.join();
    if (m_asyncThread.joinable()) m_asyncThread.join();

    // Release wakelock process if still running
    if (m_wakeLockProcess) {
        TerminateProcess(m_wakeLockProcess, 0);
        CloseHandle(m_wakeLockProcess);
        m_wakeLockProcess = nullptr;
    }

    // Stop the device server first (cleans up ADB forwarding, kills afm-server on device).
    // This must happen before kill-server, because any adb command auto-starts the daemon.
    m_device.stopServer();

    // Unmount all mounts
    DeviceMountManager::instance(0).unmount();
    DeviceMountManager::instance(1).unmount();
    McrawMountManager::instance().unmountAll();

    // Kill ADB server last — after all ADB commands are done.
    // DeviceClient::~DeviceClient() calls stopServer() again but it's a no-op (serial is cleared).
    if (m_prefs.killAdbOnClose) {
        m_device.runAdbCommand("kill-server");
    }
}

void App::asyncWorkerLoop() {
    while (!m_shutdownPoll) {
        std::function<void()> action;
        {
            std::unique_lock<std::mutex> lk(m_asyncMutex);
            m_asyncCV.wait(lk, [&]() { return m_shutdownPoll || !m_asyncQueue.empty(); });
            if (m_shutdownPoll) return;
            action = std::move(m_asyncQueue.front());
            m_asyncQueue.pop_front();
        }
        m_asyncBusy = true;
        action();
        m_asyncBusy = false;
        m_asyncStatus.clear();
    }
}

void App::postAsync(const std::string& statusMsg, std::function<void()> action) {
    {
        std::lock_guard<std::mutex> lk(m_asyncMutex);
        m_asyncStatus = statusMsg;
        m_asyncQueue.push_back(std::move(action));
    }
    m_asyncCV.notify_one();
}

void App::setupStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 3.0f;
    style.ScrollbarRounding = 4.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.WindowPadding = ImVec2(8, 8);
    style.FramePadding = ImVec2(8, 4);
    style.ItemSpacing = ImVec2(8, 6);
    style.ItemInnerSpacing = ImVec2(6, 4);
    style.ScrollbarSize = 10.0f;
    style.GrabMinSize = 8.0f;

    // --- OLED True Black Theme with Light Blue accent ---
    ImVec4* c = style.Colors;

    // Backgrounds — true black
    c[ImGuiCol_WindowBg]            = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    c[ImGuiCol_ChildBg]             = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    c[ImGuiCol_PopupBg]             = ImVec4(0.04f, 0.04f, 0.06f, 0.98f);
    c[ImGuiCol_MenuBarBg]           = ImVec4(0.03f, 0.03f, 0.05f, 1.00f);

    // Borders — subtle dark blue tint
    c[ImGuiCol_Border]              = ImVec4(0.12f, 0.14f, 0.20f, 1.00f);

    // Frame (input fields, combo boxes) — very dark with blue tint
    c[ImGuiCol_FrameBg]             = ImVec4(0.06f, 0.07f, 0.10f, 1.00f);
    c[ImGuiCol_FrameBgHovered]      = ImVec4(0.10f, 0.12f, 0.18f, 1.00f);
    c[ImGuiCol_FrameBgActive]       = ImVec4(0.14f, 0.17f, 0.25f, 1.00f);

    // Title bar
    c[ImGuiCol_TitleBg]             = ImVec4(0.02f, 0.02f, 0.04f, 1.00f);
    c[ImGuiCol_TitleBgActive]       = ImVec4(0.06f, 0.08f, 0.14f, 1.00f);

    // Scrollbar
    c[ImGuiCol_ScrollbarBg]         = ImVec4(0.02f, 0.02f, 0.03f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]       = ImVec4(0.18f, 0.22f, 0.30f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]= ImVec4(0.25f, 0.30f, 0.42f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.35f, 0.42f, 0.58f, 1.00f);

    // Buttons — dark base, light blue hover/active
    c[ImGuiCol_Button]              = ImVec4(0.08f, 0.10f, 0.15f, 1.00f);
    c[ImGuiCol_ButtonHovered]       = ImVec4(0.15f, 0.22f, 0.38f, 1.00f);
    c[ImGuiCol_ButtonActive]        = ImVec4(0.22f, 0.40f, 0.70f, 1.00f);

    // Headers (selectable rows, tree nodes) — gradient feel
    c[ImGuiCol_Header]              = ImVec4(0.10f, 0.14f, 0.22f, 0.80f);
    c[ImGuiCol_HeaderHovered]       = ImVec4(0.16f, 0.24f, 0.38f, 0.80f);
    c[ImGuiCol_HeaderActive]        = ImVec4(0.22f, 0.40f, 0.70f, 0.80f);

    // Checkmark, slider grab
    c[ImGuiCol_CheckMark]           = ImVec4(0.45f, 0.70f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrab]          = ImVec4(0.30f, 0.50f, 0.80f, 1.00f);
    c[ImGuiCol_SliderGrabActive]    = ImVec4(0.40f, 0.65f, 1.00f, 1.00f);

    // Separator
    c[ImGuiCol_Separator]           = ImVec4(0.10f, 0.12f, 0.18f, 1.00f);
    c[ImGuiCol_SeparatorHovered]    = ImVec4(0.20f, 0.30f, 0.50f, 1.00f);
    c[ImGuiCol_SeparatorActive]     = ImVec4(0.30f, 0.50f, 0.80f, 1.00f);

    // Tabs
    c[ImGuiCol_Tab]                 = ImVec4(0.05f, 0.06f, 0.09f, 1.00f);
    c[ImGuiCol_TabHovered]          = ImVec4(0.18f, 0.28f, 0.45f, 0.80f);
    c[ImGuiCol_TabSelected]         = ImVec4(0.12f, 0.20f, 0.35f, 1.00f);

    // Tables
    c[ImGuiCol_TableHeaderBg]       = ImVec4(0.04f, 0.05f, 0.08f, 1.00f);
    c[ImGuiCol_TableBorderStrong]   = ImVec4(0.10f, 0.12f, 0.18f, 1.00f);
    c[ImGuiCol_TableBorderLight]    = ImVec4(0.07f, 0.08f, 0.12f, 1.00f);
    c[ImGuiCol_TableRowBg]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TableRowBgAlt]       = ImVec4(0.06f, 0.08f, 0.12f, 0.40f);

    // Text — light blue tint (not pure white)
    c[ImGuiCol_Text]                = ImVec4(0.75f, 0.85f, 0.95f, 1.00f);
    c[ImGuiCol_TextDisabled]        = ImVec4(0.35f, 0.40f, 0.48f, 1.00f);
    c[ImGuiCol_TextSelectedBg]      = ImVec4(0.22f, 0.40f, 0.70f, 0.45f);

    // Resize grip
    c[ImGuiCol_ResizeGrip]          = ImVec4(0.15f, 0.22f, 0.35f, 0.50f);
    c[ImGuiCol_ResizeGripHovered]   = ImVec4(0.25f, 0.40f, 0.65f, 0.70f);
    c[ImGuiCol_ResizeGripActive]    = ImVec4(0.35f, 0.55f, 0.85f, 0.90f);

    // Nav highlight
    c[ImGuiCol_NavHighlight]        = ImVec4(0.30f, 0.50f, 0.80f, 1.00f);
}

void App::render() {
    ImGuiIO& io = ImGui::GetIO();

    // Reset drag state when mouse button released
    if (m_isDragging && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        m_isDragging = false;

    // Global keyboard shortcuts
    bool anyModalOpen = ImGui::IsPopupOpen("Rename") || ImGui::IsPopupOpen("New Folder") || ImGui::IsPopupOpen("Confirm Delete");

    if (ImGui::IsKeyPressed(ImGuiKey_F12)) m_showDebugWindow = !m_showDebugWindow;
    if (ImGui::IsKeyPressed(ImGuiKey_F5) && !anyModalOpen) {
        m_leftPanel.needsRefresh = true;
        m_rightPanel.needsRefresh = true;
        m_lastNavTimeLeft = std::chrono::steady_clock::now();
        m_lastNavTimeRight = std::chrono::steady_clock::now();
    }
    // F6 — copy selected files to Android (push)
    if (ImGui::IsKeyPressed(ImGuiKey_F6) && !anyModalOpen) startTransfer(false);
    // F7 — copy selected files to Windows (pull)
    if (ImGui::IsKeyPressed(ImGuiKey_F7) && !anyModalOpen) startTransfer(true);

    // Del / Shift+Del — delete selected files in the focused panel
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !anyModalOpen) {
        FilePanel* focused = m_lastFocusedPanel;
        if (focused && !focused->selectedIndices.empty()) {
            m_contextPanel = focused;
            m_contextIndex = *focused->selectedIndices.begin();
            m_deletePermanent = ImGui::GetIO().KeyShift;
            m_showDeleteConfirm = true;
        }
    }

    // Backspace — navigate up
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !anyModalOpen) {
        if (m_lastFocusedPanel) navigateUp(*m_lastFocusedPanel);
    }

    // Enter — open/navigate the focused item (same as double-click)
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) && !anyModalOpen) {
        FilePanel* focused = m_lastFocusedPanel;
        if (focused && focused->focusedIndex >= 0 && focused->validIndex(focused->focusedIndex)) {
            std::string name = focused->entryName(focused->focusedIndex);
            bool isDir = focused->entryIsDir(focused->focusedIndex);
            if (isDir) {
                std::string sep = focused->isAndroid ? "/" : "\\";
                std::string np = focused->currentPath;
                if (np.back() != sep[0]) np += sep;
                np += name;
                navigateToDirectory(*focused, np);
            } else if (endsWithCI(name, ".mcraw") && !focused->isAndroid) {
                std::string mcrawFullPath = focused->currentPath;
                if (mcrawFullPath.back() != '\\') mcrawFullPath += "\\";
                mcrawFullPath += name;
                postAsync("Mounting MCRAW...", [this, mcrawFullPath, focused]() {
                    std::string mountPath = McrawMountManager::instance().mountMcraw(mcrawFullPath);
                    if (!mountPath.empty()) {
                        focused->insideMcraw = true;
                        focused->mcrawFilePath = mcrawFullPath;
                        navigateToDirectory(*focused, mountPath);
                    }
                });
            } else if (endsWithCI(name, ".mcraw") && focused->isAndroid) {
                std::string np = focused->currentPath;
                if (np.back() != '/') np += "/";
                np += name;
                navigateToDirectory(*focused, np);
            }
        }
    }

    // Space — pause/resume active transfer
    if (ImGui::IsKeyPressed(ImGuiKey_Space) && !anyModalOpen) {
        std::lock_guard<std::mutex> lk(m_batchMutex);
        for (auto& b : m_batchQueue) {
            auto s = b->state.load();
            if (s == BatchState::Running) { b->pauseRequested = true; break; }
            if (s == BatchState::Paused)  { b->pauseRequested = false; m_batchCV.notify_all(); break; }
        }
    }

    // Ctrl+C — copy selected files to clipboard
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C) && !anyModalOpen) {
        FilePanel* focused = m_lastFocusedPanel;
        if (focused && !focused->selectedIndices.empty()) {
            m_clipboardPaths.clear();
            std::string sep = focused->isAndroid ? "/" : "\\";
            for (int idx : focused->selectedIndices) {
                if (!focused->validIndex(idx)) continue;
                std::string p = focused->currentPath;
                if (p.back() != sep[0]) p += sep;
                p += focused->entryName(idx);
                m_clipboardPaths.push_back(p);
            }
            m_clipboardIsAndroid = focused->isAndroid;
            m_clipboardDeviceSlot = focused->deviceSlot;
            m_clipboardCut = false;
        }
    }

    // Ctrl+X — cut selected files to clipboard
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X) && !anyModalOpen) {
        FilePanel* focused = m_lastFocusedPanel;
        if (focused && !focused->selectedIndices.empty()) {
            m_clipboardPaths.clear();
            std::string sep = focused->isAndroid ? "/" : "\\";
            for (int idx : focused->selectedIndices) {
                if (!focused->validIndex(idx)) continue;
                std::string p = focused->currentPath;
                if (p.back() != sep[0]) p += sep;
                p += focused->entryName(idx);
                m_clipboardPaths.push_back(p);
            }
            m_clipboardIsAndroid = focused->isAndroid;
            m_clipboardDeviceSlot = focused->deviceSlot;
            m_clipboardCut = true;
        }
    }

    // Ctrl+V — paste clipboard files into focused panel
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V) && !anyModalOpen) {
        FilePanel* focused = m_lastFocusedPanel;
        if (focused && !m_clipboardPaths.empty()) {
            performClipboardPaste(*focused);
        }
    }

    // Escape — cancel confirm dialogs
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        if (m_confirmStopTransfer) m_confirmStopTransfer = false;
        else if (m_showDeleteConfirm) m_showDeleteConfirm = false;
        else if (m_showNewFolderPopup) m_showNewFolderPopup = false;
        else if (m_showRenamePopup) m_showRenamePopup = false;
    }

    // Device polling happens in background thread - nothing blocking here

    // Refresh panels — each can be Windows or Android independently
    auto refreshPanel = [&](FilePanel& panel) {
        if (!panel.needsRefresh) return;
        if (panel.isAndroid) {
            if (m_selectedDevice < 0) return;
            // listDirectory uses the device's control channel, so it does not
            // block on transfers or drag-out drains. Refresh freely.
            refreshAndroidPanel(panel); panel.needsRefresh = false;
        } else {
            refreshWindowsPanel(panel);
            panel.needsRefresh = false;
        }
    };
    refreshPanel(m_leftPanel);
    refreshPanel(m_rightPanel);

    // --- Main full-screen window ---
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGuiWindowFlags mainFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_MenuBar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##MainWindow", nullptr, mainFlags);
    ImGui::PopStyleVar(3);

    renderMenuBar();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));

    renderDeviceBar();
    renderWifiBanner();

    float availW = ImGui::GetContentRegionAvail().x;
    float availH = ImGui::GetContentRegionAvail().y;
    float statusBarH = ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.y * 2.0f;
    float panelHeight = availH - statusBarH - ImGui::GetStyle().ItemSpacing.y;
    float panelWidth = (availW - 12.0f) * 0.5f;

    // Left panel
    ImGui::BeginChild("##LeftPanel", ImVec2(panelWidth, panelHeight), ImGuiChildFlags_Borders);
    m_leftPanelMin = ImGui::GetWindowPos();
    m_leftPanelMax = ImVec2(m_leftPanelMin.x + ImGui::GetWindowSize().x, m_leftPanelMin.y + ImGui::GetWindowSize().y);
    renderPanel(m_leftPanel, PanelSide::Left);
    // Drop target — accept drops from the other panel
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FILE_DRAG")) {
            startTransferFromDrag(m_rightPanel, m_leftPanel);
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::EndChild();

    ImGui::SameLine(0, 4.0f);
    ImGui::BeginChild("##CenterDiv", ImVec2(4.0f, panelHeight)); ImGui::EndChild();
    ImGui::SameLine(0, 4.0f);

    // Right panel
    ImGui::BeginChild("##RightPanel", ImVec2(panelWidth, panelHeight), ImGuiChildFlags_Borders);
    m_rightPanelMin = ImGui::GetWindowPos();
    m_rightPanelMax = ImVec2(m_rightPanelMin.x + ImGui::GetWindowSize().x, m_rightPanelMin.y + ImGui::GetWindowSize().y);
    renderPanel(m_rightPanel, PanelSide::Right);
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FILE_DRAG")) {
            startTransferFromDrag(m_leftPanel, m_rightPanel);
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::EndChild();

    renderStatusBar();

    ImGui::PopStyleVar();
    ImGui::End();

    // --- Transfer overlay (separate window) ---
    renderTransferOverlay();

    renderNewFolderPopup();
    renderRenamePopup();
    renderDeleteConfirmPopup();
    renderCopyMoveDialog();
    renderCrossDeviceDialog();
    renderWifiWizard();
    renderWifiPairingDialog();
    renderAboutPopup();
    renderNotificationPopup();
    renderCloseConfirmDialog();
    if (m_showPreferences) renderPreferencesWindow();
    if (m_showNicConfig) renderNicConfigWindow();
    if (m_showThemeWindow) renderThemeWindow();

    if (m_showDebugWindow) renderDebugWindow();

    // Connection mode popup
    if (m_showConnectionPopup) {
        ImGui::OpenPopup("Connection Mode");
        m_showConnectionPopup = false;
        m_connectionPopupScanned = false;
        m_detectedIps.clear();
        // Scan IPs asynchronously when popup opens
        std::string scanSerial;
        {
            std::lock_guard<std::mutex> lk(m_deviceMutex);
            if (m_selectedDevice >= 0 && m_selectedDevice < (int)m_devices.size())
                scanSerial = m_devices[m_selectedDevice].serial;
        }
        if (!scanSerial.empty()) {
            postAsync("Scanning device IPs...", [this, scanSerial]() {
                const char* ifaces[] = {"rndis0", "usb0", "ncm0"};
                const char* labels[] = {"USB Tethering (rndis0)", "USB (usb0)", "USB NCM (ncm0)"};
                std::vector<DetectedIp> found;
                for (int i = 0; i < 3; i++) {
                    std::string cmd = "-s " + scanSerial + " shell \"ip -4 addr show " + std::string(ifaces[i]) + " 2>/dev/null | grep inet\"";
                    std::string out = m_device.runAdbCommand(cmd);
                    auto pos = out.find("inet ");
                    if (pos == std::string::npos) continue;
                    pos += 5;
                    auto slash = out.find('/', pos);
                    auto space = out.find(' ', pos);
                    auto endp = std::min(slash, space);
                    if (endp == std::string::npos || endp <= pos) continue;
                    std::string ip = out.substr(pos, endp - pos);
                    if (ip == "127.0.0.1") continue;
                    found.push_back({ ifaces[i], labels[i], ip });
                }
                m_detectedIps = std::move(found);
                m_connectionPopupScanned = true;
            });
        } else {
            m_connectionPopupScanned = true;
        }
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Connection Mode", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Current mode:");
        if (m_device.isDirectConnection()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.3f, 1, 0.5f, 1), "Direct TCP: %s", m_device.deviceIp().c_str());
        } else {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1), "ADB Forward");
        }

        ImGui::Separator();
        ImGui::Spacing();

        // Show detected IPs (scanned asynchronously)
        ImGui::Text("Detected device IPs:");
        ImGui::Spacing();

        std::string popupSerial;
        {
            std::lock_guard<std::mutex> lk(m_deviceMutex);
            if (m_selectedDevice >= 0 && m_selectedDevice < (int)m_devices.size())
                popupSerial = m_devices[m_selectedDevice].serial;
        }
        if (!m_connectionPopupScanned) {
            ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Scanning...");
        } else if (!popupSerial.empty()) {
            bool foundAny = !m_detectedIps.empty();
            for (auto& det : m_detectedIps) {
                ImGui::BulletText("%s: %s", det.label.c_str(), det.ip.c_str());
                ImGui::SameLine();

                bool isCurrent = m_device.isDirectConnection() && m_device.deviceIp() == det.ip;
                if (isCurrent) {
                    ImGui::TextColored(ImVec4(0.3f, 1, 0.5f, 1), "(active)");
                } else {
                    std::string btnId = "Connect##" + det.iface;
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.35f, 0.20f, 1));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.50f, 0.28f, 1));
                    if (ImGui::SmallButton(btnId.c_str())) {
                        std::string connectIp = det.ip;
                        std::string ser = popupSerial;
                        ImGui::CloseCurrentPopup();
                        postAsync("Connecting to " + connectIp + "...", [this, connectIp, ser]() {
                            if (m_device.connectDirect(connectIp)) {
                                m_statusMessage = "Direct TCP: " + connectIp;
                            } else {
                                m_device.startServer(ser);
                                m_statusMessage = "Failed to connect to " + connectIp;
                            }
                            m_statusTime = std::chrono::steady_clock::now();
                        });
                    }
                    ImGui::PopStyleColor(2);
                }
            }

            if (!foundAny) {
                ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "No network interfaces found.");
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.35f, 0.20f, 1));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.50f, 0.28f, 1));
                if (ImGui::Button("Enable USB Tethering", ImVec2(-1, 28))) {
                    std::string ser = popupSerial;
                    ImGui::CloseCurrentPopup();
                    postAsync("Enabling USB tethering...", [this, ser]() {
                        if (m_device.tryEnableUsbTethering(ser)) {
                            m_statusMessage = "USB Tethering enabled!";
                        } else {
                            m_statusMessage = "Could not enable tethering - enable manually in device Settings";
                        }
                        m_statusTime = std::chrono::steady_clock::now();
                    });
                }
                ImGui::PopStyleColor(2);
                ImGui::TextDisabled("Or enable manually: Settings > Network > Hotspot > USB Tethering");
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Manual IP input
        ImGui::Text("Manual IP:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        bool enterIp = ImGui::InputText("##ManualIP", m_manualIp, sizeof(m_manualIp), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((ImGui::Button("Connect##manual") || enterIp) && strlen(m_manualIp) > 0) {
            std::string connectIp(m_manualIp);
            std::string ser = popupSerial;
            ImGui::CloseCurrentPopup();
            postAsync("Connecting to " + connectIp + "...", [this, connectIp, ser]() {
                if (m_device.connectDirect(connectIp)) {
                    m_statusMessage = "Direct TCP: " + connectIp;
                } else {
                    if (!ser.empty()) m_device.startServer(ser);
                    m_statusMessage = "Failed to connect to " + connectIp;
                }
                m_statusTime = std::chrono::steady_clock::now();
            });
        }

        ImGui::Spacing();

        // Switch back to ADB forward
        if (m_device.isDirectConnection()) {
            if (ImGui::Button("Switch to ADB Forward", ImVec2(-1, 0))) {
                std::string ser = popupSerial;
                ImGui::CloseCurrentPopup();
                postAsync("Switching to ADB Forward...", [this, ser]() {
                    m_device.disconnectTcp();
                    // Set up ADB forward and connect directly — don't use startServer which would pick Direct TCP again
                    m_device.runAdbCommand("-s " + ser + " forward tcp:" + std::to_string(AFM_PORT) + " tcp:" + std::to_string(AFM_PORT));
                    if (m_device.connectTcp("127.0.0.1", AFM_PORT) && m_device.verifyConnection()) {
                        m_statusMessage = "Switched to ADB Forward";
                    } else {
                        m_device.disconnectTcp();
                        m_statusMessage = "ADB Forward failed — reconnecting...";
                        m_device.startServer(ser);
                    }
                    m_statusTime = std::chrono::steady_clock::now();
                });
            }
        }

        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(-1, 28))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Update Windows taskbar progress
    {
        bool anyActive = false;
        float bestProg = 0;
        BatchState bestState = BatchState::Queued;
        {
            std::lock_guard<std::mutex> lk(m_batchMutex);
            for (auto& b : m_batchQueue) {
                auto s = b->state.load();
                if (s == BatchState::Running || s == BatchState::Paused || s == BatchState::Verifying || s == BatchState::WaitingConflict) {
                    anyActive = true;
                    bestProg = b->totalProgress.load();
                    bestState = s;
                    break;
                }
            }
        }
        if (anyActive) {
            if (bestState == BatchState::Paused || bestState == BatchState::WaitingConflict)
                setTaskbarPaused();
            else
                updateTaskbarProgress(bestProg, true);
        } else {
            // Check if last batch failed
            bool lastFailed = false;
            {
                std::lock_guard<std::mutex> lk(m_batchMutex);
                if (!m_batchQueue.empty()) {
                    auto s = m_batchQueue.back()->state.load();
                    if (s == BatchState::Failed) lastFailed = true;
                }
            }
            if (lastFailed)
                setTaskbarError();
            else
                updateTaskbarProgress(0, false);
        }
    }

    m_firstFrame = false;
}

void App::renderMenuBar() {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Refresh All", "F5")) {
                m_leftPanel.needsRefresh = true;
                m_rightPanel.needsRefresh = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                if (m_prefs.confirmOnClose) {
                    m_showCloseConfirm = true;
                } else {
                    g_reallyQuit = true;
                    PostMessage(g_mainHwnd, WM_CLOSE, 0, 0);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Preferences...")) {
                m_showPreferences = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Show Hidden (Windows)", nullptr, &m_leftPanel.showHidden);
            ImGui::MenuItem("Show Hidden (Android)", nullptr, &m_rightPanel.showHidden);
            ImGui::Separator();
            if (ImGui::MenuItem("Show Transfer Window", nullptr, m_overlayVisible)) {
                m_overlayVisible = !m_overlayVisible;
            }
            if (ImGui::MenuItem("Appearance...")) {
                m_showThemeWindow = true;
            }
            ImGui::MenuItem("Debug Log", "F12", &m_showDebugWindow);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Transfer")) {
            if (ImGui::MenuItem("Copy to Android  >>", "F6")) startTransfer(false);
            if (ImGui::MenuItem("<< Copy to Windows", "F7")) startTransfer(true);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Connection")) {
            bool hasDevice = false;
            std::string curSerial;
            {
                std::lock_guard<std::mutex> lk(m_deviceMutex);
                if (m_selectedDevice >= 0 && m_selectedDevice < (int)m_devices.size() && m_devices[m_selectedDevice].state == "device") {
                    hasDevice = true;
                    curSerial = m_devices[m_selectedDevice].serial;
                }
            }

            if (m_device.isServerRunning()) {
                if (m_device.isDirectConnection())
                    ImGui::TextColored(ImVec4(0.3f,1,0.5f,1), "Direct TCP: %s", m_device.deviceIp().c_str());
                else
                    ImGui::TextColored(ImVec4(0.7f,0.7f,0.7f,1), "ADB Forward");
                ImGui::Separator();
            }

            if (m_asyncBusy) {
                ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Working: %s", m_asyncStatus.c_str());
            } else if (hasDevice) {
                bool isDirect = m_device.isDirectConnection();
                bool isConnected = m_device.isServerRunning();

#if 0 // Hidden: superseded by USB 2x / WiFi 2x multi-pipe options
                if (ImGui::MenuItem("Connection Mode...")) {
                    m_showConnectionPopup = true;
                }
#endif
                if (ImGui::MenuItem("Multi-NIC Configuration...")) {
                    m_showNicConfig = true;
                    if (m_detectedNics.empty())
                        m_detectedNics = enumerateNics();
                    m_nicTestResults.assign(m_detectedNics.size(), false);
                    m_nicTestDone.assign(m_detectedNics.size(), false);
                    m_nicTestReason.assign(m_detectedNics.size(), "");
                }
#if 0 // Hidden: superseded by USB 2x / WiFi 2x multi-pipe options. Logic preserved.
                ImGui::Separator();

                // Switch between connection types
                if (isDirect) {
                    if (ImGui::MenuItem("Switch to ADB Forward")) {
                        m_preferAdbForward = true;
                        std::string ser = curSerial;
                        postAsync("Switching to ADB Forward...", [this, ser]() {
                            m_device.disconnectTcp();
                            m_device.runAdbCommand("-s " + ser + " forward tcp:" + std::to_string(AFM_PORT) + " tcp:" + std::to_string(AFM_PORT));
                            if (m_device.connectTcp("127.0.0.1", AFM_PORT) && m_device.verifyConnection()) {
                                m_statusMessage = "Switched to ADB Forward";
                            } else {
                                m_device.disconnectTcp();
                                m_statusMessage = "ADB Forward failed";
                                m_device.startServer(ser);
                            }
                            m_statusTime = std::chrono::steady_clock::now();
                        });
                    }
                } else {
                    if (ImGui::MenuItem("Switch to Direct TCP")) {
                        m_preferAdbForward = false;
                        std::string ser = curSerial;
                        postAsync("Switching to Direct TCP...", [this, ser]() {
                            LOG_INFO("UI", "Manual switch to Direct TCP");
                            std::string ip = m_device.detectDeviceIp(ser);
                            // No tethering IP? Enable it first
                            if (ip.empty()) {
                                LOG_INFO("UI", "No tethering IP — enabling tethering");
                                m_tetheringInProgress = true;
                                m_device.tryEnableUsbTethering(ser);
                                m_tetheringInProgress = false;
                                ip = m_device.detectDeviceIp(ser);
                            }
                            if (!ip.empty()) {
                                if (m_device.connectDirect(ip)) {
                                    m_statusMessage = "Direct TCP: " + ip;
                                } else {
                                    m_statusMessage = "Direct TCP failed: " + m_device.lastError();
                                }
                            } else {
                                m_statusMessage = "No USB tethering IP found — tethering may not be supported";
                            }
                            m_statusTime = std::chrono::steady_clock::now();
                        });
                    }
                }

                ImGui::Separator();

                if (ImGui::MenuItem("Enable USB Tethering")) {
                    std::string ser = curSerial;
                    postAsync("Enabling USB tethering...", [this, ser]() {
                        LOG_INFO("UI", "Manual tethering enable");
                        if (m_device.tryEnableUsbTethering(ser)) {
                            m_statusMessage = "USB tethering enabled";
                            std::string ip = m_device.detectDeviceIp(ser);
                            if (!ip.empty() && m_device.connectDirect(ip))
                                m_statusMessage = "Tethering + Direct TCP: " + ip;
                        } else {
                            m_statusMessage = "Tethering unavailable";
                        }
                        m_statusTime = std::chrono::steady_clock::now();
                    });
                }
#endif

                ImGui::Separator();

                // Dokan virtual drive mount (per device slot)
                for (int slot = 0; slot < 2; slot++) {
                    if (!m_slotConnected[slot]) continue;
                    auto& mgr = DeviceMountManager::instance(slot);
                    std::string driveLetter = (slot == 0) ? "P:\\" : "Q:\\";
                    std::string slotLabel = m_slotSerial[slot];
                    if (slotLabel.size() > 12) slotLabel = slotLabel.substr(0, 12) + "..";

                    if (mgr.isMounted()) {
                        std::string mp = mgr.mountPoint();
                        ImGui::TextColored(ImVec4(0.3f,1,0.5f,1), "Mounted: %s (%s)", mp.c_str(), slotLabel.c_str());
                        std::string unmountId = "Unmount " + mp + "##" + std::to_string(slot);
                        if (ImGui::MenuItem(unmountId.c_str())) {
                            postAsync("Unmounting...", [slot]() {
                                DeviceMountManager::instance(slot).unmount();
                            });
                        }
                        std::string openId = "Open " + mp + " in Explorer##" + std::to_string(slot);
                        if (ImGui::MenuItem(openId.c_str())) {
                            ShellExecuteA(nullptr, "explore", mp.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                        }
                    } else {
                        std::string mountId = "Mount " + slotLabel + " as " + driveLetter + "##" + std::to_string(slot);
                        if (ImGui::MenuItem(mountId.c_str())) {
                            std::string sr = m_slotStorageRoot[slot];
                            postAsync("Mounting virtual drive...", [this, slot, sr, driveLetter]() {
                                if (DeviceMountManager::instance(slot).mount(&m_deviceSlots[slot], sr, driveLetter)) {
                                    m_statusMessage = "Mounted: " + driveLetter;
                                    ShellExecuteA(nullptr, "explore", driveLetter.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                                } else {
                                    m_statusMessage = "Mount failed — is Dokan driver installed?";
                                }
                                m_statusTime = std::chrono::steady_clock::now();
                            });
                        }
                    }
                }

                ImGui::Separator();

                // WiFi ADB options
                if (ImGui::BeginMenu("WiFi ADB")) {
                    if (ImGui::MenuItem("Setup Wizard...", nullptr, false, hasDevice)) {
                        m_showWifiWizard = true;
                        m_wizardStep = 0;
                    }
                    if (ImGui::MenuItem("Pair (Android 11+)...")) {
                        m_showWifiPairing = true;
                        m_pairingIp[0] = '\0';
                        m_pairingCode[0] = '\0';
                    }
                    ImGui::Separator();
                    if (!m_prefs.savedWifiDevices.empty()) {
                        ImGui::TextDisabled("Saved devices:");
                        for (int i = 0; i < (int)m_prefs.savedWifiDevices.size(); i++) {
                            auto& w = m_prefs.savedWifiDevices[i];
                            std::string label = w.wifiIp + ":" + std::to_string(w.port);
                            if (!w.serial.empty()) label += " (" + w.serial.substr(0, 8) + ")";
                            ImGui::PushID(i);
                            if (ImGui::MenuItem(label.c_str())) {
                                // Manual connect to saved device
                                std::string ip = w.wifiIp;
                                int port = w.port;
                                postAsync("Connecting WiFi...", [this, ip, port]() {
                                    std::string result = m_device.runAdbCommand("connect " + ip + ":" + std::to_string(port));
                                    m_statusMessage = result;
                                    m_statusTime = std::chrono::steady_clock::now();
                                });
                            }
                            ImGui::SameLine();
                            if (ImGui::SmallButton("X")) {
                                m_prefs.savedWifiDevices.erase(m_prefs.savedWifiDevices.begin() + i);
                                m_prefs.save();
                            }
                            ImGui::PopID();
                        }
                    }
                    ImGui::EndMenu();
                }

                ImGui::Separator();

                if (ImGui::MenuItem("Reconnect Server")) {
                    std::string ser = curSerial;
                    bool preferForward = m_preferAdbForward;
                    postAsync("Reconnecting server...", [this, ser, preferForward]() {
                        LOG_INFO("UI", std::string("Manual reconnect (prefer ") + (preferForward ? "ADB Forward" : "Direct TCP") + ")");
                        m_device.stopServer();
                        if (preferForward) {
                            // ADB Forward path — don't use startServer which prefers Direct TCP
                            m_device.runAdbCommand("-s " + ser + " shell killall afm-server 2>/dev/null");
                            std::this_thread::sleep_for(std::chrono::milliseconds(500));
                            // Push and start server
                            std::string serverBin = m_device.getServerBinaryPath();
                            if (std::filesystem::exists(toFsPath(serverBin)))
                                m_device.runAdbCommand("-s " + ser + " push \"" + serverBin + "\" /data/local/tmp/afm-server");
                            m_device.runAdbCommand("-s " + ser + " shell chmod 755 /data/local/tmp/afm-server");
                            std::string cmd = "\"" + m_device.getAdbPath() + "\" -s " + ser +
                                " shell \"nohup /data/local/tmp/afm-server " + std::to_string(AFM_PORT) + " > /dev/null 2>&1 &\"";
                            std::string cmdBuf = cmd;
                            STARTUPINFOA si{}; si.cb = sizeof(si);
                            si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
                            PROCESS_INFORMATION pi{};
                            CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
                            if (pi.hProcess) { WaitForSingleObject(pi.hProcess, 3000); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
                            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                            // Connect via ADB forward
                            m_device.runAdbCommand("-s " + ser + " forward tcp:" + std::to_string(AFM_PORT) + " tcp:" + std::to_string(AFM_PORT));
                            if (m_device.connectTcp("127.0.0.1", AFM_PORT) && m_device.verifyConnection()) {
                                m_statusMessage = "Reconnected via ADB Forward";
                            } else {
                                m_device.disconnectTcp();
                                m_statusMessage = "Reconnect failed";
                            }
                        } else {
                            m_device.startServer(ser);
                            m_statusMessage = m_device.isServerRunning()
                                ? ("Reconnected via " + std::string(m_device.isDirectConnection() ? "Direct TCP" : "ADB Forward"))
                                : ("Reconnect failed: " + m_device.lastError());
                        }
                        m_statusTime = std::chrono::steady_clock::now();
                    });
                }

                if (ImGui::MenuItem("Restart ADB Daemon")) {
                    postAsync("Restarting ADB daemon...", [this]() {
                        LOG_INFO("UI", "Manual ADB daemon restart");
                        m_device.runAdbCommand("kill-server");
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        m_device.runAdbCommand("start-server");
                        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                        m_statusMessage = "ADB daemon restarted";
                        m_statusTime = std::chrono::steady_clock::now();
                    });
                }
            } else {
                ImGui::TextDisabled("No device connected");

                if (ImGui::MenuItem("Restart ADB Daemon")) {
                    postAsync("Restarting ADB daemon...", [this]() {
                        LOG_INFO("UI", "Manual ADB daemon restart");
                        m_device.runAdbCommand("kill-server");
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        m_device.runAdbCommand("start-server");
                        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                        m_statusMessage = "ADB daemon restarted";
                        m_statusTime = std::chrono::steady_clock::now();
                    });
                }
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About")) {
                m_showAboutPopup = true;
            }
            ImGui::EndMenu();
        }

        float tw = ImGui::CalcTextSize("Fast Enough? - Android File Explorer").x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - tw) * 0.5f);
        ImGui::TextColored(ImVec4(0.5f,0.7f,1,1), "Fast Enough? - Android File Explorer");
        ImGui::EndMenuBar();
    }
}

void App::renderDeviceBar() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.02f,0.02f,0.04f,1));
    float devBarH = ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.y * 2 + 8;
    ImGui::BeginChild("##DeviceBar", ImVec2(0, devBarH));
    ImGui::SetCursorPos(ImVec2(8, 8));

    if (m_device.getAdbPath().empty()) {
        ImGui::TextColored(ImVec4(1,0.3f,0.3f,1), "ADB not found!");
        ImGui::SameLine(); ImGui::TextDisabled("Install Android SDK or add adb.exe to PATH");
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(0.5f,0.7f,1,1), "Device:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(300);

        // Snapshot under lock to avoid race with poll thread
        std::vector<DeviceInfo> devSnap;
        int selSnap;
        {
            std::lock_guard<std::mutex> lk(m_deviceMutex);
            devSnap = m_devices;
            selSnap = m_selectedDevice;
        }

        if (devSnap.empty()) {
            ImGui::TextColored(ImVec4(1,0.6f,0.2f,1), "No device connected");
        } else {
            // Use marketing name if available, otherwise fall back to adb model
            auto getDevName = [&](int idx) -> std::string {
                auto it = m_deviceDisplayNames.find(devSnap[idx].serial);
                const std::string& name = (it != m_deviceDisplayNames.end()) ? it->second : devSnap[idx].model;
                return name + " (" + devSnap[idx].serial + ")";
            };
            std::string preview = selSnap >= 0 ? getDevName(selSnap) : "Select...";
            if (ImGui::BeginCombo("##DeviceCombo", preview.c_str())) {
                for (int i = 0; i < (int)devSnap.size(); i++) {
                    std::string label = getDevName(i);
                    if (devSnap[i].state != "device") {
                        label += " [" + devSnap[i].state + "]";
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,0.5f,0.5f,1));
                    }
                    if (ImGui::Selectable(label.c_str(), i == selSnap)) {
                        std::lock_guard<std::mutex> lk(m_deviceMutex);
                        m_selectedDevice = i;
                        m_lastDeviceSerial.clear(); // force re-detection
                    }
                    if (devSnap[i].state != "device") ImGui::PopStyleColor();
                }
                ImGui::EndCombo();
            }
            if (selSnap >= 0 && devSnap[selSnap].state == "device") {
                ImGui::SameLine();

                // Show connection mode - detect actual transport
                if (m_device.isServerRunning()) {
                    bool serialIsWifi = isWifiSerial(devSnap[selSnap].serial);
                    if (m_dualChannelAvailable) {
                        if (m_activeChannelCount >= 4)
                            ImGui::TextColored(ImVec4(0.3f,1,0.5f,1), "%d channels", m_activeChannelCount);
                        else if (serialIsWifi)
                            ImGui::TextColored(ImVec4(0.3f,0.8f,1,1), "WiFi (dual)");
                        else if (m_secondaryChannelType == "USB")
                            ImGui::TextColored(ImVec4(0.3f,1,0.5f,1), "USB (dual)");
                        else
                            ImGui::TextColored(ImVec4(0.3f,1,0.5f,1), "USB + %s", m_secondaryChannelType.c_str());
                    } else if (serialIsWifi) {
                        ImGui::TextColored(ImVec4(0.3f,0.8f,1,1), "WiFi");
                    } else if (m_device.isDirectConnection()) {
                        ImGui::TextColored(ImVec4(0.3f,1,0.5f,1), "USB (Direct)");
                    } else {
                        ImGui::TextColored(ImVec4(0.7f,0.7f,0.7f,1), "USB");
                    }
                } else {
                    ImGui::TextColored(ImVec4(1,0.6f,0.2f,1), "Connecting...");
                }

                // Pipe toggles — directly visible for quick tuning
                ImGui::SameLine(0, 16);
                {
                    bool usbDual = (m_prefs.usbPipeCount == 2);
                    if (ImGui::Checkbox("USB x2##devbar", &usbDual)) {
                        m_prefs.usbPipeCount = usbDual ? 2 : 1;
                        m_prefs.save();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("USB dual-pipe: two parallel ADB connections\nfor ~2x USB throughput (USB 3.x recommended).\nApplies on next device reconnect.");
                }
                ImGui::SameLine(0, 8);
                {
                    bool wifiDual = (m_prefs.wifiPipeCount == 2);
                    if (ImGui::Checkbox("WiFi x2##devbar", &wifiDual)) {
                        m_prefs.wifiPipeCount = wifiDual ? 2 : 1;
                        m_prefs.save();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("WiFi dual-pipe: two parallel TCP connections\nfor ~2x WiFi throughput (WiFi 6/7 recommended).\nApplies on next device reconnect.");
                }

                // WiFi auto-connect toggle
                ImGui::SameLine(0, 16);
                bool wifiAuto = m_prefs.wifiAutoConnect;
                if (ImGui::Checkbox("WiFi Auto##devbar", &wifiAuto)) {
                    m_prefs.wifiAutoConnect = wifiAuto;
                    m_prefs.save();
                    if (!wifiAuto) {
                        // Disconnect secondary channel and disable dual-channel
                        m_secondaryChannel.disconnectTcp();
                        m_dualChannelAvailable = false;
                        m_secondaryChannelType.clear();
                        m_wifiAutoSetupDone = false;
                        m_statusMessage = "WiFi auto-connect disabled";
                        m_statusTime = std::chrono::steady_clock::now();
                    }
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Automatically set up WiFi for dual-channel transfers\nwhen this device connects via USB");

                // Keep awake toggle (uses a wakelock held by a background adb shell process)
                ImGui::SameLine(0, 16);
                bool keepAwake = m_keepAwake;
                if (ImGui::Checkbox("Keep Awake##devbar", &keepAwake)) {
                    std::string serial = devSnap[selSnap].serial;
                    std::string adbPath = m_device.getAdbPath();
                    if (keepAwake && !m_keepAwake) {
                        // Spawn a background adb shell that holds a wakelock
                        // When the process dies (disconnect, app close), the lock auto-releases
                        std::string cmd = "\"" + adbPath + "\" -s " + serial +
                            " shell \"echo afm_keepawake > /sys/power/wake_lock && cat > /dev/null\"";
                        STARTUPINFOA si{}; si.cb = sizeof(si);
                        si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
                        PROCESS_INFORMATION pi{};
                        if (CreateProcessA(nullptr, (LPSTR)cmd.c_str(), nullptr, nullptr, FALSE,
                                           CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                            CloseHandle(pi.hThread);
                            m_wakeLockProcess = pi.hProcess;
                            m_keepAwake = true;
                            m_statusMessage = "Keep awake enabled (wakelock)";
                            m_statusTime = std::chrono::steady_clock::now();
                        } else {
                            // Fallback for non-root: use svc power stayon
                            m_device.runAdbCommand("-s " + serial + " shell svc power stayon usb");
                            cmd = "\"" + adbPath + "\" -s " + serial +
                                " shell \"trap 'svc power stayon false' EXIT; cat > /dev/null\"";
                            si = {}; si.cb = sizeof(si);
                            si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
                            pi = {};
                            if (CreateProcessA(nullptr, (LPSTR)cmd.c_str(), nullptr, nullptr, FALSE,
                                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                                CloseHandle(pi.hThread);
                                m_wakeLockProcess = pi.hProcess;
                            }
                            m_keepAwake = true;
                            m_statusMessage = "Keep awake enabled";
                            m_statusTime = std::chrono::steady_clock::now();
                        }
                    } else if (!keepAwake && m_keepAwake) {
                        // Kill the background process, which releases the wakelock
                        if (m_wakeLockProcess) {
                            TerminateProcess(m_wakeLockProcess, 0);
                            CloseHandle(m_wakeLockProcess);
                            m_wakeLockProcess = nullptr;
                        }
                        m_keepAwake = false;
                        // Clean up wakelock entry async to avoid UI hang
                        postAsync("Disabling keep awake...", [this, serial]() {
                            m_device.runAdbCommand("-s " + serial + " shell \"echo afm_keepawake > /sys/power/wake_unlock 2>/dev/null; svc power stayon false 2>/dev/null\"");
                            m_statusMessage = "Keep awake disabled";
                            m_statusTime = std::chrono::steady_clock::now();
                        });
                    }
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Prevent the device from sleeping while connected.\nUses a wakelock that auto-releases if the connection drops.");

                // (Dual channel button removed — replaced by USB x2 / WiFi x2 toggles above)

                // Disconnect button for WiFi devices
                if (isWifiSerial(devSnap[selSnap].serial)) {
                    ImGui::SameLine(0, 16);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.15f, 0.15f, 1));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.2f, 0.2f, 1));
                    if (ImGui::SmallButton("Disconnect##wifi")) {
                        std::string serial = devSnap[selSnap].serial;
                        // Kill wakelock process if active (will auto-release on device)
                        if (m_keepAwake && m_wakeLockProcess) {
                            TerminateProcess(m_wakeLockProcess, 0);
                            CloseHandle(m_wakeLockProcess);
                            m_wakeLockProcess = nullptr;
                            m_keepAwake = false;
                        }
                        postAsync("Disconnecting...", [this, serial]() {
                            m_device.stopServer();
                            m_device.runAdbCommand("disconnect " + serial);
                            m_statusMessage = "Disconnected: " + serial;
                            m_statusTime = std::chrono::steady_clock::now();
                        });
                    }
                    ImGui::PopStyleColor(2);
                }
            }
        }

    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void App::renderPanel(FilePanel& panel, PanelSide side) {
    const char* label = panel.isAndroid ? "Android" : "Windows";
    ImVec4 labelColor = panel.isAndroid ? ImVec4(0.3f,0.85f,0.4f,1) : ImVec4(0.40f,0.65f,1,1);

    // If Android panel but no device connected, show setup guide
    if (panel.isAndroid && !deviceFor(panel).isServerRunning()) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.0f);

        float availH = ImGui::GetContentRegionAvail().y;
        float centerY = availH * 0.3f;
        ImGui::SetCursorPosY(centerY);

        // Center content
        float availW = ImGui::GetContentRegionAvail().x;
        float contentW = 360.0f;
        float padX = (availW - contentW) * 0.5f;
        if (padX > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padX);

        ImGui::BeginGroup();

        ImGui::TextColored(ImVec4(0.45f, 0.70f, 1, 1), "No Android device connected");
        ImGui::Spacing();
        ImGui::Spacing();

        // Show other connected devices the user can switch to
        {
            std::lock_guard<std::mutex> lk(m_deviceMutex);
            int onlineCount = 0;
            for (auto& d : m_devices) if (d.state == "device") onlineCount++;
            if (onlineCount > 0) {
                ImGui::TextDisabled("Available devices:");
                ImGui::Spacing();
                for (int i = 0; i < (int)m_devices.size(); i++) {
                    if (m_devices[i].state != "device") continue;
                    bool isSel = (m_selectedDevice == i);
                    std::string label = m_devices[i].serial;
                    if (!m_devices[i].model.empty()) label = m_devices[i].model + " (" + m_devices[i].serial + ")";
                    if (isSel) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.35f, 0.55f, 1));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.45f, 0.70f, 1));
                    }
                    if (ImGui::SmallButton(label.c_str())) {
                        m_selectedDevice = i;
                        m_lastDeviceSerial.clear();
                        panel.needsRefresh = true;
                    }
                    if (isSel) ImGui::PopStyleColor(2);
                }
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
            }
        }

        // Show saved WiFi devices the user can connect to directly
        if (!m_prefs.savedWifiDevices.empty()) {
            ImGui::TextDisabled("Saved WiFi devices:");
            ImGui::Spacing();
            for (int si = 0; si < (int)m_prefs.savedWifiDevices.size(); si++) {
                auto& w = m_prefs.savedWifiDevices[si];
                if (w.wifiIp.empty()) continue;
                std::string connectAddr = w.wifiIp + ":" + std::to_string(w.port);
                std::string displayLabel;
                if (!w.model.empty())
                    displayLabel = w.model + " (" + connectAddr + ")";
                else if (!w.serial.empty() && w.serial != connectAddr)
                    displayLabel = w.serial + " (" + connectAddr + ")";
                else
                    displayLabel = connectAddr;
                std::string btnLabel = "Connect##saved" + std::to_string(si);
                // Auto-connect on launch checkbox
                std::string autoLabel = "Auto##auto" + std::to_string(si);
                if (ImGui::Checkbox(autoLabel.c_str(), &w.autoConnect)) {
                    m_prefs.save();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Automatically connect to this device\nwhen the app launches");
                ImGui::SameLine();
                if (ImGui::SmallButton(btnLabel.c_str())) {
                    int savedIdx = si;
                    postAsync("Connecting to " + connectAddr + "...", [this, connectAddr, savedIdx]() {
                        std::string result = m_device.runAdbCommand("connect " + connectAddr);
                        if (result.find("connected") != std::string::npos &&
                            result.find("failed") == std::string::npos) {
                            m_statusMessage = "Connected: " + connectAddr;
                            m_prefs.wifiAutoConnect = true;
                            // Backfill model if missing
                            if (savedIdx < (int)m_prefs.savedWifiDevices.size() &&
                                m_prefs.savedWifiDevices[savedIdx].model.empty()) {
                                std::string displayName = queryDeviceDisplayName(connectAddr);
                                if (!displayName.empty()) {
                                    m_prefs.savedWifiDevices[savedIdx].model = displayName;
                                    m_prefs.save();
                                }
                            }
                        } else {
                            m_statusMessage = "Connect failed: " + result;
                        }
                        m_statusTime = std::chrono::steady_clock::now();
                    });
                }
                ImGui::SameLine();
                ImGui::Text("%s", displayLabel.c_str());
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        }

        ImGui::TextDisabled("1. Connect your phone via USB cable");
        ImGui::TextDisabled("2. Enable Developer Options and USB Debugging");
        ImGui::TextDisabled("3. When prompted, tap Allow to authorize this computer");
        ImGui::Spacing();
        ImGui::TextDisabled("For wireless transfers, use the setup wizard below.");
        ImGui::Spacing();
        ImGui::Spacing();

        // Calculate button width from the widest label
        float btn1W = ImGui::CalcTextSize("Run Setup Wizard").x + ImGui::GetStyle().FramePadding.x * 2;
        float btn2W = ImGui::CalcTextSize("WiFi ADB Pairing (Android 11+)").x + ImGui::GetStyle().FramePadding.x * 2;
        float btnW = std::max(btn1W, btn2W);

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.35f, 0.55f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.45f, 0.70f, 1));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (contentW - btnW) * 0.5f);
        if (ImGui::Button("Run Setup Wizard", ImVec2(btnW, 0))) {
            m_showWifiWizard = true;
            m_wizardStep = 0;
        }
        ImGui::PopStyleColor(2);

        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.40f, 0.35f, 0.10f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.55f, 0.50f, 0.15f, 1));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (contentW - btnW) * 0.5f);
        if (ImGui::Button("WiFi ADB Pairing (Android 11+)", ImVec2(btnW, 0))) {
            m_showWifiPairing = true;
            m_pairingIp[0] = '\0';
            m_pairingCode[0] = '\0';
        }
        ImGui::PopStyleColor(2);

        ImGui::Spacing();
        ImGui::Spacing();

        // Centered "Waiting for device..."
        {
            char waitText[32];
            float t = (float)fmod(ImGui::GetTime() * 0.8, 1.0);
            int dots = (int)(t * 4) % 4;
            snprintf(waitText, sizeof(waitText), "Waiting for device%.*s", dots, "...");
            float textW = ImGui::CalcTextSize(waitText).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (contentW - textW) * 0.5f);
            ImGui::TextDisabled("%s", waitText);
        }

        ImGui::EndGroup();
        ImGui::PopStyleVar();
        return;
    }

    // Fade-in animation after navigation
    {
        auto& navTime = (side == PanelSide::Left) ? m_lastNavTimeLeft : m_lastNavTimeRight;
        float elapsed = (float)std::chrono::duration<double>(std::chrono::steady_clock::now() - navTime).count();
        float alpha = std::min(elapsed / 0.15f, 1.0f); // 150ms fade
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * alpha);
    }

    // --- Navigation bar: Back / Forward / Up + Breadcrumb ---
    bool canBack = panel.navHistoryPos > 0;
    bool canFwd = panel.navHistoryPos >= 0 && panel.navHistoryPos < (int)panel.navHistory.size() - 1;

    auto triggerNavAnim = [&]() {
        if (side == PanelSide::Left) m_lastNavTimeLeft = std::chrono::steady_clock::now();
        else m_lastNavTimeRight = std::chrono::steady_clock::now();
    };

    ImGui::BeginDisabled(!canBack);
    if (ImGui::SmallButton(("<##B" + std::string(label)).c_str())) {
        panel.navHistoryPos--;
        panel.currentPath = panel.navHistory[panel.navHistoryPos];
        panel.selectedIndices.clear(); panel.focusedIndex = -1;
        panel.searchFilter[0] = '\0'; panel.needsRefresh = true;
        strcpy_s(panel.pathInput, panel.currentPath.c_str());
        triggerNavAnim();
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0, 2);

    ImGui::BeginDisabled(!canFwd);
    if (ImGui::SmallButton((">##F" + std::string(label)).c_str())) {
        panel.navHistoryPos++;
        panel.currentPath = panel.navHistory[panel.navHistoryPos];
        panel.selectedIndices.clear(); panel.focusedIndex = -1;
        panel.searchFilter[0] = '\0'; panel.needsRefresh = true;
        strcpy_s(panel.pathInput, panel.currentPath.c_str());
        triggerNavAnim();
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0, 4);

    if (ImGui::SmallButton(("^##U" + std::string(label)).c_str())) navigateUp(panel);
    ImGui::SameLine(0, 6);

    // Panel mode toggle: Windows / Android
    {
        std::string winId = std::string("PC##") + (side == PanelSide::Left ? "L" : "R");
        std::string andId = std::string("Android##") + (side == PanelSide::Left ? "L" : "R");

        if (!panel.isAndroid) {
            // Windows is active — show it highlighted, Android as clickable
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.22f, 0.38f, 1));
            ImGui::SmallButton(winId.c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine(0, 2);
            if (ImGui::SmallButton(andId.c_str())) switchPanelMode(panel, true);
        } else {
            // Android is active
            if (ImGui::SmallButton(winId.c_str())) switchPanelMode(panel, false);
            ImGui::SameLine(0, 2);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.30f, 0.15f, 1));
            ImGui::SmallButton(andId.c_str());
            ImGui::PopStyleColor();

            // Device indicator / selector - always visible when in Android mode
            {
                std::lock_guard<std::mutex> lk(m_deviceMutex);

                // Count online devices
                int onlineCount = 0;
                for (auto& d : m_devices) if (d.state == "device") onlineCount++;

                // Helper: get friendly display name for a device slot
                auto getSlotName = [&](int slot) -> std::string {
                    if (slot < 0 || slot >= 2 || !m_slotConnected[slot]) return "No device";
                    auto it = m_deviceDisplayNames.find(m_slotSerial[slot]);
                    if (it != m_deviceDisplayNames.end()) return it->second;
                    for (auto& d : m_devices) {
                        if (d.serial == m_slotSerial[slot])
                            return d.model.empty() ? d.serial : d.model;
                    }
                    return m_slotSerial[slot];
                };

                // Slot colors for visual distinction between devices
                static const ImVec4 slotColors[] = {
                    ImVec4(0.35f, 0.65f, 1.0f, 1.0f),  // slot 0: blue
                    ImVec4(0.2f,  0.85f, 0.5f, 1.0f),   // slot 1: green
                };

                int slotIdx = panel.deviceSlot;
                ImVec4 slotColor = slotColors[slotIdx < 2 ? slotIdx : 0];

                if (onlineCount >= 1 && m_slotConnected[slotIdx]) {
                    ImGui::SameLine(0, 8);

                    // Draw colored dot indicator
                    ImVec2 dotPos = ImGui::GetCursorScreenPos();
                    dotPos.x += 4.0f;
                    dotPos.y += ImGui::GetFrameHeight() * 0.5f;
                    ImGui::GetWindowDrawList()->AddCircleFilled(dotPos, 4.0f,
                        ImGui::ColorConvertFloat4ToU32(slotColor));
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14);

                    std::string currentName = getSlotName(slotIdx);

                    if (onlineCount > 1) {
                        // Multiple devices: dropdown to switch
                        std::string comboId = "##DevSlot" + std::string(side == PanelSide::Left ? "L" : "R");
                        ImGui::SetNextItemWidth(200);
                        if (ImGui::BeginCombo(comboId.c_str(), currentName.c_str())) {
                            for (int di = 0; di < 2; di++) {
                                if (!m_slotConnected[di]) continue;
                                bool isSel = (panel.deviceSlot == di);
                                std::string devName = getSlotName(di);
                                // Show serial in parentheses for disambiguation, unique ImGui ID per slot
                                std::string fullLabel = devName + "  (" + m_slotSerial[di] + ")##slot" + std::to_string(di);

                                ImVec4 itemColor = slotColors[di < 2 ? di : 0];
                                if (isSel) ImGui::PushStyleColor(ImGuiCol_Text, itemColor);

                                if (ImGui::Selectable(fullLabel.c_str(), isSel)) {
                                    if (panel.deviceSlot != di) {
                                        panel.deviceSlot = di;
                                        panel.currentPath = m_slotStorageRoot[di].empty() ? "/" : m_slotStorageRoot[di];
                                        strcpy_s(panel.pathInput, panel.currentPath.c_str());
                                        panel.needsRefresh = true;
                                        panel.navHistory.clear();
                                        panel.navHistoryPos = -1;
                                    }
                                }
                                if (isSel) ImGui::PopStyleColor();
                            }
                            ImGui::EndCombo();
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Switch which device this panel browses");
                    } else {
                        // Single device: show name as colored label
                        ImGui::TextColored(slotColor, "%s", currentName.c_str());
                    }
                }
            }
        }
    }
    ImGui::SameLine(0, 6);

    // Breadcrumb path — clickable segments
    {
        char sep = panel.isAndroid ? '/' : '\\';
        std::string path = panel.currentPath;
        std::vector<std::pair<std::string, std::string>> crumbs; // {segment, full path}

        if (panel.isAndroid) {
            if (!path.empty() && path[0] == '/') {
                crumbs.push_back({"/", "/"});
                path = path.substr(1);
            }
            std::string accum = "/";
            size_t pos = 0;
            while (pos < path.size()) {
                size_t next = path.find('/', pos);
                if (next == std::string::npos) next = path.size();
                std::string seg = path.substr(pos, next - pos);
                if (!seg.empty()) {
                    accum += seg;
                    crumbs.push_back({seg, accum});
                    accum += "/";
                }
                pos = next + 1;
            }
        } else {
            // Windows: "C:\Users\Foo" -> ["C:\", "Users", "Foo"]
            if (path.size() >= 2 && path[1] == ':') {
                std::string root = path.substr(0, 3); // "C:\"
                crumbs.push_back({root, root});
                path = (path.size() > 3) ? path.substr(3) : "";
            }
            std::string accum = crumbs.empty() ? "" : crumbs[0].second;
            size_t pos = 0;
            while (pos < path.size()) {
                size_t next = path.find('\\', pos);
                if (next == std::string::npos) next = path.size();
                std::string seg = path.substr(pos, next - pos);
                if (!seg.empty()) {
                    if (accum.back() != '\\') accum += "\\";
                    accum += seg;
                    crumbs.push_back({seg, accum});
                }
                pos = next + 1;
            }
        }

        for (size_t ci = 0; ci < crumbs.size(); ci++) {
            if (ci > 0) { ImGui::SameLine(0, 0); ImGui::TextDisabled("%c", sep); ImGui::SameLine(0, 0); }
            ImGui::PushID((int)ci);
            if (ci < crumbs.size() - 1) {
                // Clickable
                if (ImGui::SmallButton(crumbs[ci].first.c_str()))
                    navigateToDirectory(panel, crumbs[ci].second);
            } else {
                // Current (non-clickable, highlighted)
                ImGui::TextColored(ImVec4(0.5f,0.75f,1,1), "%s", crumbs[ci].first.c_str());
            }
            ImGui::PopID();
        }
    }

    // Filter bar
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint(("##Filter" + std::string(label)).c_str(), "Search / filter...", panel.searchFilter, sizeof(panel.searchFilter));
    ImGui::Separator();

    if (!panel.isAndroid) {
        for (const auto& d : getWindowsDrives()) { if (ImGui::SmallButton(d.c_str())) navigateToDirectory(panel, d + "\\"); ImGui::SameLine(); }
        ImGui::NewLine();
    } else {
        // Dynamic bookmarks based on detected storage
        std::string root = m_androidStorageRoot.empty() ? "/sdcard" : m_androidStorageRoot;

        // Storage volumes (internal + SD cards)
        for (size_t vi = 0; vi < m_androidVolumes.size(); vi++) {
            std::string label = (vi == 0) ? "Internal" : "SD " + std::to_string(vi);
            if (ImGui::SmallButton(label.c_str())) navigateToDirectory(panel, m_androidVolumes[vi]);
            ImGui::SameLine();
        }
        if (m_androidVolumes.empty()) {
            if (ImGui::SmallButton("Storage")) navigateToDirectory(panel, root);
            ImGui::SameLine();
        }

        // Common subfolders
        const char* subfolders[] = {"Download", "DCIM", "Documents", "Pictures", "Music"};
        for (const char* sub : subfolders) {
            if (ImGui::SmallButton(sub)) navigateToDirectory(panel, root + "/" + sub);
            ImGui::SameLine();
        }

        // Root access button
        if (ImGui::SmallButton("/")) navigateToDirectory(panel, "/");
        ImGui::SameLine();
        ImGui::NewLine();
    }

    // Ctrl+A — select all
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && ImGui::IsKeyPressed(ImGuiKey_A) && ImGui::GetIO().KeyCtrl) {
        for (int i = 0; i < panel.entryCount(); i++) panel.selectedIndices.insert(i);
    }

    ImGuiTableFlags tf = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Sortable;
    if (ImGui::BeginTable(("##Tbl" + std::string(label)).c_str(), 3, tf)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort, 0.55f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 0.20f);
        ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthStretch, 0.25f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        // Handle sort specs
        if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
            if (specs->SpecsDirty && specs->SpecsCount > 0) {
                int col = specs->Specs[0].ColumnIndex;
                bool desc = specs->Specs[0].SortDirection == ImGuiSortDirection_Descending;
                panel.sortColumn = col;
                panel.sortDescending = desc;
                // Re-sort entries
                if (panel.isAndroid) {
                    std::sort(panel.androidEntries.begin(), panel.androidEntries.end(),
                        [col, desc](const DeviceFileEntry& a, const DeviceFileEntry& b) {
                            if (a.isDirectory() != b.isDirectory()) return a.isDirectory();
                            int cmp;
                            if (col == 1) cmp = (a.size < b.size) ? -1 : (a.size > b.size) ? 1 : 0;
                            else if (col == 2) cmp = (a.mtime < b.mtime) ? -1 : (a.mtime > b.mtime) ? 1 : 0;
                            else cmp = _stricmp(a.name.c_str(), b.name.c_str());
                            return desc ? cmp > 0 : cmp < 0;
                        });
                } else {
                    std::sort(panel.windowsEntries.begin(), panel.windowsEntries.end(),
                        [col, desc](const WindowsFileEntry& a, const WindowsFileEntry& b) {
                            if (a.isDirectory != b.isDirectory) return a.isDirectory > b.isDirectory;
                            int cmp;
                            if (col == 1) cmp = (a.size < b.size) ? -1 : (a.size > b.size) ? 1 : 0;
                            else if (col == 2) cmp = a.dateModified.compare(b.dateModified);
                            else cmp = _stricmp(a.name.c_str(), b.name.c_str());
                            return desc ? cmp > 0 : cmp < 0;
                        });
                }
                specs->SpecsDirty = false;
            }
        }

        // ".." - hide at drive root (e.g. "C:\") or filesystem root ("/")
        bool atRoot = false;
        if (!panel.isAndroid) {
            // Windows: at root if path is "X:\" (3 chars)
            atRoot = (panel.currentPath.size() <= 3 && panel.currentPath.size() >= 2 && panel.currentPath[1] == ':');
        } else {
            atRoot = (panel.currentPath == "/");
        }
        if (!atRoot) {
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            if (ImGui::Selectable("[..]##up", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
                if (ImGui::IsMouseDoubleClicked(0)) navigateUp(panel);
            ImGui::TableNextColumn(); ImGui::TableNextColumn();
        }

        std::string filter(panel.searchFilter);
        std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

        // Track row screen positions for rubber-band selection
        struct RowInfo { int index; float yMin, yMax; };
        std::vector<RowInfo> visibleRows;

        int count = panel.entryCount();
        for (int i = 0; i < count; i++) {
            std::string name = panel.entryName(i);
            if (!filter.empty()) {
                std::string nl = name;
                std::transform(nl.begin(), nl.end(), nl.begin(), ::tolower);
                if (nl.find(filter) == std::string::npos) continue;
            }
            if (!panel.showHidden && !panel.isAndroid) {
                if (!name.empty() && name[0] == '.') continue;
                if (i < (int)panel.windowsEntries.size() && panel.windowsEntries[i].isHidden) continue;
            }

            bool isDir = panel.entryIsDir(i);
            bool isSel = panel.selectedIndices.count(i) > 0;

            ImGui::TableNextRow(); ImGui::TableNextColumn();

            // Track row screen position for rubber-band
            float rowYMin = ImGui::GetCursorScreenPos().y;

            std::string icon = getFileIcon(name, isDir);
            std::string sid = icon + " " + name + "##" + std::to_string(i);

            if (isDir) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f,0.70f,1,1));
            if (isSel) ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.15f,0.28f,0.50f,0.90f));

            ImGuiSelectableFlags sf = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick;
            if (ImGui::Selectable(sid.c_str(), isSel, sf)) {
                ImGuiIO& io = ImGui::GetIO();
                if (io.KeyCtrl) { if (isSel) panel.selectedIndices.erase(i); else panel.selectedIndices.insert(i); }
                else if (io.KeyShift && panel.focusedIndex >= 0) {
                    int lo = std::min(panel.focusedIndex, i), hi = std::max(panel.focusedIndex, i);
                    for (int j = lo; j <= hi; j++) panel.selectedIndices.insert(j);
                } else { panel.selectedIndices.clear(); panel.selectedIndices.insert(i); }
                panel.focusedIndex = i;
                m_lastFocusedPanel = &panel;

                if (ImGui::IsMouseDoubleClicked(0)) {
                    if (isDir) {
                        std::string sep = panel.isAndroid ? "/" : "\\";
                        std::string np = panel.currentPath;
                        if (np.back() != sep[0]) np += sep;
                        np += name;
                        navigateToDirectory(panel, np);
                    } else if (endsWithCI(name, ".mcraw") && !panel.isAndroid) {
                        // Windows side: mount MCRAW via ProjFS and navigate to mount point
                        std::string mcrawFullPath = panel.currentPath;
                        if (mcrawFullPath.back() != '\\') mcrawFullPath += "\\";
                        mcrawFullPath += name;
                        FilePanel* targetPanel = &panel;
                        postAsync("Mounting MCRAW...", [this, mcrawFullPath, targetPanel]() {
                            std::string mountPath = McrawMountManager::instance().mountMcraw(mcrawFullPath);
                            if (!mountPath.empty()) {
                                targetPanel->insideMcraw = true;
                                targetPanel->mcrawFilePath = mcrawFullPath;
                                navigateToDirectory(*targetPanel, mountPath);
                            } else {
                                m_statusMessage = "Failed to mount MCRAW";
                                m_statusTime = std::chrono::steady_clock::now();
                            }
                        });
                    } else if (endsWithCI(name, ".mcraw") && panel.isAndroid) {
                        // Android side: navigate into MCRAW as virtual directory (protocol-based)
                        std::string np = panel.currentPath;
                        if (np.back() != '/') np += "/";
                        np += name;
                        navigateToDirectory(panel, np);
                    } else if (panel.insideMcraw && panel.isAndroid) {
                        // Double-clicked virtual item inside Android MCRAW — pull to temp and open
                        std::string vname = name;
                        std::string mcraw = panel.mcrawFilePath;
                        postAsync("Extracting " + vname + "...", [this, mcraw, vname]() {
                            char tmpDir[MAX_PATH];
                            GetTempPathA(MAX_PATH, tmpDir);
                            std::string tmpPath = std::string(tmpDir) + vname;
                            uint64_t outSize = 0;
                            if (m_device.pullMcrawItem(mcraw, vname, tmpPath, outSize)) {
                                ShellExecuteA(nullptr, "open", tmpPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                            } else {
                                m_statusMessage = "Failed to extract " + vname;
                                m_statusTime = std::chrono::steady_clock::now();
                            }
                        });
                    } else if (!panel.isAndroid) {
                        // Open file on Windows side
                        std::string fp = panel.currentPath;
                        if (fp.back() != '\\') fp += "\\";
                        fp += name;
                        ShellExecuteA(nullptr, "open", fp.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    } else {
                        // Double-clicked a file on Android side — pull to temp and open
                        openAndroidFile(panel, i);
                    }
                }
            }

            // --- Drag source — auto-select item if not already selected ---
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                if (panel.selectedIndices.count(i) == 0) {
                    panel.selectedIndices.clear();
                    panel.selectedIndices.insert(i);
                    panel.focusedIndex = i;
                }
                DragPayload dp;
                dp.isAndroid = panel.isAndroid;
                ImGui::SetDragDropPayload("FILE_DRAG", &dp, sizeof(dp));
                int selCount = (int)panel.selectedIndices.size();
                if (selCount == 1) {
                    ImGui::Text("Move: %s", name.c_str());
                } else {
                    ImGui::Text("Move %d files", selCount);
                }
                m_dragSourcePanel = &panel;
                m_isDragging = true;
                ImGui::EndDragDropSource();
            }

            if (isSel) ImGui::PopStyleColor(); // header
            if (isDir) ImGui::PopStyleColor(); // text

            // Right-click context - preserve multi-selection if clicking an already-selected item
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                if (panel.selectedIndices.count(i) == 0) {
                    // Clicked outside selection - select only this item
                    panel.selectedIndices.clear();
                    panel.selectedIndices.insert(i);
                }
                panel.focusedIndex = i; m_contextPanel = &panel; m_contextIndex = i;
                ImGui::OpenPopup("##FileContextMenu");
            }

            ImGui::TableNextColumn();
            if (!isDir) ImGui::TextDisabled("%s", formatSize(panel.entrySize(i)).c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", panel.entryDate(i).c_str());

            // Record row extent for rubber-band
            float rowYMax = ImGui::GetCursorScreenPos().y;
            visibleRows.push_back({i, rowYMin, rowYMax});
        }

        renderContextMenu(panel);

        // Right-click on empty space in the table area — background context menu
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !ImGui::IsAnyItemHovered() &&
            ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
            m_contextPanel = &panel;
            m_contextIndex = -1;
            panel.selectedIndices.clear();
            m_lastFocusedPanel = &panel;
            ImGui::OpenPopup("##BgContextMenu");
        }
        if (ImGui::BeginPopup("##BgContextMenu")) {
            if (m_contextPanel == &panel) {
                if (ImGui::MenuItem("New Folder...")) {
                    memset(m_newFolderName, 0, sizeof(m_newFolderName));
                    m_contextPanel = &panel; m_showNewFolderPopup = true;
                }
                if (!m_clipboardPaths.empty()) {
                    const char* pasteLabel = m_clipboardCut ? "Move Here" : "Paste";
                    if (ImGui::MenuItem(pasteLabel, "Ctrl+V")) {
                        performClipboardPaste(panel);
                    }
                }
            }
            ImGui::EndPopup();
        }

        ImGui::EndTable();

        // --- Rubber-band (lasso) selection ---
        ImVec2 tableMin = ImGui::GetItemRectMin();
        ImVec2 tableMax = ImGui::GetItemRectMax();
        ImVec2 mousePos = ImGui::GetMousePos();

        // Start rubber-band on mouse down in the table area (not on a Selectable)
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().KeyCtrl &&
            mousePos.x >= tableMin.x && mousePos.x <= tableMax.x &&
            mousePos.y >= tableMin.y && mousePos.y <= tableMax.y &&
            !ImGui::IsAnyItemHovered() && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
            panel.rubberBandActive = true;
            panel.rubberBandStart = mousePos;
            panel.rubberBandEnd = mousePos;
            if (!ImGui::GetIO().KeyShift)
                panel.selectedIndices.clear();
            m_lastFocusedPanel = &panel;
        }

        if (panel.rubberBandActive) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                panel.rubberBandEnd = mousePos;

                // Calculate selection rect (normalized)
                float selMinY = std::min(panel.rubberBandStart.y, panel.rubberBandEnd.y);
                float selMaxY = std::max(panel.rubberBandStart.y, panel.rubberBandEnd.y);
                float selMinX = std::min(panel.rubberBandStart.x, panel.rubberBandEnd.x);
                float selMaxX = std::max(panel.rubberBandStart.x, panel.rubberBandEnd.x);

                // Select rows that overlap the rubber-band rect
                panel.selectedIndices.clear();
                for (auto& row : visibleRows) {
                    if (row.yMax > selMinY && row.yMin < selMaxY) {
                        panel.selectedIndices.insert(row.index);
                    }
                }

                // Draw the rubber-band rectangle
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(
                    ImVec2(selMinX, selMinY), ImVec2(selMaxX, selMaxY),
                    IM_COL32(60, 120, 220, 50));
                dl->AddRect(
                    ImVec2(selMinX, selMinY), ImVec2(selMaxX, selMaxY),
                    IM_COL32(80, 140, 240, 180));
            } else {
                panel.rubberBandActive = false;
            }
        }
    }

    ImGui::PopStyleVar(); // alpha fade
}

void App::drawProgressBar(float fraction, const char* overlayText, float height,
                           ImVec4 barColor, ImVec4 bgColor) {
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barColor);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, bgColor);
    ImGui::ProgressBar(fraction, ImVec2(-1, height), "");
    ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    ImVec2 ts = ImGui::CalcTextSize(overlayText);
    ImVec2 tp(mn.x + (mx.x - mn.x - ts.x) * 0.5f, mn.y + (mx.y - mn.y - ts.y) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(ImVec2(tp.x+1, tp.y+1), IM_COL32(0,0,0,180), overlayText);
    ImGui::GetWindowDrawList()->AddText(tp, IM_COL32(255,255,255,240), overlayText);
    ImGui::PopStyleColor(2);
}

void App::renderTransferOverlay() {
    std::shared_ptr<TransferBatch> batch;
    int queueSize = 0;
    {
        std::lock_guard<std::mutex> lk(m_batchMutex);
        queueSize = (int)m_batchQueue.size();
        // Find active or most recent batch
        for (auto& b : m_batchQueue) {
            BatchState s = b->state.load();
            if (s == BatchState::Running || s == BatchState::Paused || s == BatchState::Verifying || s == BatchState::WaitingConflict) { batch = b; break; }
        }
        // If none active, show the last completed/stopped one
        if (!batch && !m_batchQueue.empty()) {
            batch = m_batchQueue.back();
        }
    }

    if (!batch) {
        m_overlayVisible = false;
        return;
    }

    // Auto-show overlay when a batch starts
    BatchState st = batch->state.load();
    if (st == BatchState::Running || st == BatchState::Paused || st == BatchState::Queued || st == BatchState::Verifying || st == BatchState::WaitingConflict) {
        if (!m_overlayWasOpen) { m_overlayVisible = true; m_overlayWasOpen = true; }
    }

    // Auto-dismiss on success (no CRC failures)
    if (st == BatchState::Completed && m_prefs.autoDismissTransfer && m_overlayVisible) {
        if (batch->crcFailCount() == 0) {
            m_overlayVisible = false;
            m_overlayWasOpen = false;
            std::lock_guard<std::mutex> lk(m_batchMutex);
            while (!m_batchQueue.empty()) {
                BatchState s = m_batchQueue.front()->state.load();
                if (s == BatchState::Completed || s == BatchState::Failed || s == BatchState::Stopped)
                    m_batchQueue.pop_front();
                else break;
            }
        }
    }

    // Smooth fade for overlay
    if (m_overlayVisible)
        m_overlayFade = std::min(m_overlayFade + ImGui::GetIO().DeltaTime * 6.0f, 1.0f);
    else
        m_overlayFade = std::max(m_overlayFade - ImGui::GetIO().DeltaTime * 8.0f, 0.0f);

    if (m_overlayFade <= 0.01f && !m_overlayVisible) {
        // Show a small indicator in status bar instead (handled in renderStatusBar)
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, m_overlayFade);

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    // Auto-size: fixed width, height expands to fit content
    ImVec2 vpSize = ImGui::GetMainViewport()->Size;
    static float trackedWidth = 520.0f;
    // Track the max width this window has needed (only grows, never shrinks during a batch)
    // Use content width to ensure text/bars always fit
    float minW = std::max(trackedWidth, (batch->useMultiNic || batch->useParallelChannels) ? 650.0f : 520.0f);
    ImGui::SetNextWindowSizeConstraints(ImVec2(minW, 0), ImVec2(vpSize.x * 0.9f, vpSize.y * 0.85f));

    ImGuiWindowFlags wf = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize;
    bool open = m_overlayVisible;
    ImGui::Begin("Transfer Progress", &open, wf);
    m_overlayVisible = open; // user closed via X

    float totalProg   = batch->totalProgress.load();
    float curFileProg = batch->curFileProgress.load();
    uint64_t totalBytes = batch->totalBytes.load();
    uint64_t totalTx    = batch->totalTransferred.load();
    uint64_t curFSize   = batch->curFileSize.load();
    uint64_t curFTx     = batch->curFileTransferred.load();
    double speed  = batch->speedBytesPerSec.load();
    double eta    = batch->etaSeconds.load();
    int curIdx    = batch->currentFileIndex.load();
    int totalFiles = batch->totalFiles();
    std::string verb = batch->isMove ? "Move" : "Copy";
    std::string direction;
    if (batch->isCrossDevice)
        direction = "Copy: Android -> Android (direct relay)";
    else if (batch->isLocalCopy)
        direction = verb + ": Windows -> Windows";
    else if (batch->isPull)
        direction = verb + ": Android -> Windows";
    else
        direction = verb + ": Windows -> Android";
    if (batch->useMultiNic)
        direction += " [" + std::to_string(batch->numChannels) + " NICs]";
    else if (batch->useParallelChannels)
        direction += " [dual channel]";

    // Header
    ImGui::TextColored(ImVec4(0.5f,0.7f,1,1), "%s", direction.c_str());
    ImGui::SameLine();
    if (queueSize > 1) ImGui::TextDisabled("(%d batches queued)", queueSize);

    ImGui::Separator();

    // --- Current file / per-channel progress ---
    if (st == BatchState::Running || st == BatchState::Paused || st == BatchState::WaitingConflict) {
        if (batch->useParallelChannels || batch->useMultiNic) {
            // Multi-channel: show separate progress for each channel
            ImVec4 chColors[] = {
                {0.25f, 0.55f, 0.95f, 0.9f}, // blue
                {0.30f, 0.75f, 0.40f, 0.9f}, // green
                {0.85f, 0.55f, 0.25f, 0.9f}, // orange
                {0.75f, 0.35f, 0.85f, 0.9f}, // purple
                {0.85f, 0.75f, 0.25f, 0.9f}, // yellow
                {0.35f, 0.80f, 0.80f, 0.9f}, // cyan
                {0.85f, 0.35f, 0.45f, 0.9f}, // red
                {0.55f, 0.75f, 0.55f, 0.9f}, // light green
            };

            int nCh = batch->numChannels;
            for (int ch = 0; ch < nCh; ch++) {
                auto& cp = batch->channels[ch];
                if (cp.channelName.empty() && cp.filesCompleted.load() == 0 && cp.bytesTransferred.load() == 0) continue;

                double spd = cp.speed.load();
                uint64_t tx = cp.bytesTransferred.load();

                // Show current file being transferred by this channel
                std::string curFile = cp.currentFile;
                bool isActive = (spd > 0.01 || !curFile.empty());

                // Hide channels that are permanently dead
                if (!isActive && tx == 0) continue;

                // Channel name + speed on one line
                ImGui::TextColored(chColors[ch % 8], "%s", cp.channelName.c_str());
                ImGui::SameLine();
                if (spd > 0.01)
                    ImGui::Text("%s  %s", formatSize(tx).c_str(), formatSpeed(spd).c_str());
                else if (tx > 0)
                    ImGui::TextDisabled("%s  (idle)", formatSize(tx).c_str());
                else
                    ImGui::TextDisabled("(connecting...)");

                // Current file for this channel
                if (!curFile.empty() && curFile[0] != '(') {
                    ImGui::SameLine();
                    ImGui::TextDisabled("  %s", curFile.c_str());
                }
            }
        } else {
            // Single-channel: show current file
            std::string curName = (curIdx < totalFiles) ? batch->files[curIdx].displayName : "";
            ImGui::Text("File %d / %d:", curIdx + 1, totalFiles);
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.8f,0.9f,1,1), "%s", curName.c_str());

            char curOvl[256];
            snprintf(curOvl, sizeof(curOvl), "%s / %s  (%.1f%%)",
                     formatSize(curFTx).c_str(), formatSize(curFSize).c_str(), curFileProg * 100.0f);
            drawProgressBar(curFileProg, curOvl, 20.0f,
                            ImVec4(0.25f,0.55f,0.95f,0.9f), ImVec4(0.04f,0.04f,0.06f,1));

            ImGui::Spacing();
        }
    }

    // --- Total progress ---
    ImGui::Text("Total:");
    ImGui::SameLine();
    ImGui::TextDisabled("%d files, %s", totalFiles, formatSize(totalBytes).c_str());

    char totalOvl[256];
    if (st == BatchState::Completed) {
        double avgSpd = batch->finalAvgSpeed.load();
        double totalSec = batch->finalTimeSec.load();
        snprintf(totalOvl, sizeof(totalOvl), "%s / %s  -  %s avg  -  Done in %s",
                 formatSize(totalTx).c_str(), formatSize(totalBytes).c_str(),
                 formatSpeed(avgSpd).c_str(), formatETA(totalSec).c_str());
        drawProgressBar(1.0f, totalOvl, 22.0f,
                        ImVec4(0.2f,0.7f,0.3f,0.9f), ImVec4(0.04f,0.04f,0.06f,1));

        // Per-channel summary for parallel transfers
        if ((batch->useParallelChannels || batch->useMultiNic) && batch->numChannels > 1) {
            ImGui::Spacing();
            int nCh = batch->numChannels;
            for (int ch = 0; ch < nCh; ch++) {
                auto& cp = batch->channels[ch];
                if (cp.bytesTransferred.load() == 0 && cp.filesCompleted.load() == 0) continue;
                double sec = cp.elapsedSec.load();
                if (sec < 0.01) sec = 0.01;
                double spd = (double)cp.bytesTransferred.load() / sec;
                ImGui::TextDisabled("  %s: %d blocks, %s in %s (%s avg)",
                    cp.channelName.c_str(), cp.filesCompleted.load(),
                    formatSize(cp.bytesTransferred.load()).c_str(),
                    formatETA(sec).c_str(), formatSpeed(spd).c_str());
            }
        }
    } else if (st == BatchState::Failed || st == BatchState::Stopped) {
        snprintf(totalOvl, sizeof(totalOvl), "%s / %s  -  %s",
                 formatSize(totalTx).c_str(), formatSize(totalBytes).c_str(),
                 st == BatchState::Stopped ? "Stopped" : "Failed");
        drawProgressBar(totalProg, totalOvl, 22.0f,
                        ImVec4(0.7f,0.2f,0.2f,0.9f), ImVec4(0.04f,0.04f,0.06f,1));
    } else if (st == BatchState::Verifying) {
        // Transfer done, CRC verification in progress — two phases
        int phase = batch->crcPhase.load();
        float crcProg = batch->crcProgress.load();

        if (phase == 1) {
            // Phase 1: server computing — animated pulse since we have no server-side progress
            float t = (float)fmod(ImGui::GetTime() * 0.8, 1.0);
            snprintf(totalOvl, sizeof(totalOvl), "Verifying: %s  -  Device computing CRC...",
                     batch->crcFileName.c_str());
            drawProgressBar(t, totalOvl, 22.0f,
                            ImVec4(0.5f,0.35f,0.8f,0.7f), ImVec4(0.04f,0.04f,0.06f,1));
        } else if (phase == 2) {
            // Phase 2: local CRC — real progress
            snprintf(totalOvl, sizeof(totalOvl), "Verifying: %s  -  Local CRC %.0f%%",
                     batch->crcFileName.c_str(), crcProg * 100.0f);
            drawProgressBar(crcProg, totalOvl, 22.0f,
                            ImVec4(0.5f,0.4f,0.9f,0.9f), ImVec4(0.04f,0.04f,0.06f,1));
        } else {
            snprintf(totalOvl, sizeof(totalOvl), "Verifying: %s",
                     batch->crcFileName.c_str());
            drawProgressBar(0.0f, totalOvl, 22.0f,
                            ImVec4(0.5f,0.4f,0.9f,0.9f), ImVec4(0.04f,0.04f,0.06f,1));
        }
    } else {
        std::string etaStr = (eta > 0) ? ("  ETA: " + formatETA(eta)) : "";
        std::string speedStr = formatSpeed(speed);

        // For push with no estimate: show animated total bar too
        snprintf(totalOvl, sizeof(totalOvl), "%s / %s  (%.1f%%)  -  %s%s",
                 formatSize(totalTx).c_str(), formatSize(totalBytes).c_str(),
                 totalProg * 100.0f, speedStr.c_str(), etaStr.c_str());
        float barFrac = totalProg;
        ImVec4 barCol;
        if (batch->disconnected.load()) barCol = ImVec4(0.8f,0.4f,0.1f,0.9f); // orange when disconnected
        else if (st == BatchState::Paused || st == BatchState::WaitingConflict) barCol = ImVec4(0.7f,0.6f,0.1f,0.9f);
        else barCol = ImVec4(0.25f,0.55f,0.95f,0.9f);
        drawProgressBar(barFrac, totalOvl, 22.0f, barCol, ImVec4(0.04f,0.04f,0.06f,1));
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // --- Control buttons ---
    bool isRunning = (st == BatchState::Running);
    bool isPaused  = (st == BatchState::Paused);
    bool isDone    = (st == BatchState::Completed || st == BatchState::Failed || st == BatchState::Stopped);
    bool waitingRetry = batch->waitingForUserRetry.load();
    bool waitingConflict = batch->waitingConflict.load();

    if (waitingConflict) {
        // File already exists — ask user what to do
        ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "File already exists:");
        ImGui::TextColored(ImVec4(0.9f, 0.9f, 1, 1), "  %s", batch->conflictFileName.c_str());
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.35f, 0.55f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.45f, 0.70f, 1));
        if (ImGui::Button("Overwrite", ImVec2(100, 0))) {
            batch->conflictResponse = ConflictAction::Overwrite;
            m_batchCV.notify_all();
        }
        ImGui::SameLine();
        if (ImGui::Button("Overwrite All", ImVec2(120, 0))) {
            batch->conflictResponse = ConflictAction::OverwriteAll;
            m_batchCV.notify_all();
        }
        ImGui::PopStyleColor(2);

        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.40f, 0.35f, 0.10f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.55f, 0.50f, 0.15f, 1));
        if (ImGui::Button("Skip", ImVec2(100, 0))) {
            batch->conflictResponse = ConflictAction::Skip;
            m_batchCV.notify_all();
        }
        ImGui::SameLine();
        if (ImGui::Button("Skip All", ImVec2(120, 0))) {
            batch->conflictResponse = ConflictAction::SkipAll;
            m_batchCV.notify_all();
        }
        ImGui::PopStyleColor(2);

    } else if (waitingRetry) {
        // Waiting for user decision after retries exhausted
        ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "Reconnection failed. Retry or cancel?");
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.35f, 0.20f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.50f, 0.28f, 1));
        if (ImGui::Button("Retry", ImVec2(120, 0))) {
            batch->userRetryRequested = true;
            m_batchCV.notify_all();
        }
        ImGui::PopStyleColor(2);

        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.12f, 0.12f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.18f, 0.18f, 1));
        if (ImGui::Button("Cancel Transfer", ImVec2(140, 0))) {
            batch->stopRequested = true;
            batch->userRetryRequested = false;
            // Stop is handled by progress callback checking stopRequested every chunk
            m_batchCV.notify_all();
        }
        ImGui::PopStyleColor(2);

    } else if (batch->disconnected.load() && !isDone) {
        // Device disconnected during transfer - show status and manual retry
        ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "Device disconnected!");
        ImGui::Spacing();
        if (!batch->errorMessage.empty())
            ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", batch->errorMessage.c_str());
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.35f, 0.20f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.50f, 0.28f, 1));
        if (ImGui::Button("Force Retry Now", ImVec2(140, 0))) {
            // Signal the reconnection loop to try immediately
            batch->errorMessage = "User requested retry...";
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.12f, 0.12f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.18f, 0.18f, 1));
        if (ImGui::Button("Cancel Transfer", ImVec2(140, 0))) {
            batch->stopRequested = true;
            batch->userRetryRequested = false;
            // Stop is handled by progress callback checking stopRequested every chunk
            m_batchCV.notify_all();
        }
        ImGui::PopStyleColor(2);

    } else if (!isDone) {
        if (isPaused) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f,0.35f,0.20f,1));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f,0.50f,0.28f,1));
            if (ImGui::Button("Resume", ImVec2(90, 0))) {
                batch->pauseRequested = false;
                m_batchCV.notify_all();
            }
            ImGui::PopStyleColor(2);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.40f,0.35f,0.10f,1));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.55f,0.50f,0.15f,1));
            if (ImGui::Button("Pause", ImVec2(90, 0))) {
                batch->pauseRequested = true;
            }
            ImGui::PopStyleColor(2);
        }

        ImGui::SameLine();
        if (!m_confirmStopTransfer) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f,0.12f,0.12f,1));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f,0.18f,0.18f,1));
            if (ImGui::Button("Stop", ImVec2(90, 0))) {
                m_confirmStopTransfer = true;
            }
            ImGui::PopStyleColor(2);
        } else {
            ImGui::TextColored(ImVec4(1,0.6f,0.2f,1), "Stop transfer?");
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f,0.12f,0.12f,1));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f,0.18f,0.18f,1));
            if (ImGui::Button("Yes", ImVec2(60, 0))) {
                batch->stopRequested = true;
                batch->pauseRequested = false;
                // Stop is handled by progress callback checking stopRequested every chunk
                m_batchCV.notify_all();
                m_confirmStopTransfer = false;
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            if (ImGui::Button("No", ImVec2(60, 0))) {
                m_confirmStopTransfer = false;
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Hide", ImVec2(90, 0))) {
            m_overlayVisible = false;
        }
    } else {
        // Done - show close + maybe clear
        if (ImGui::Button("Close", ImVec2(90, 0))) {
            m_overlayVisible = false;
            m_overlayWasOpen = false;
            // Clean up completed batches
            std::lock_guard<std::mutex> lk(m_batchMutex);
            while (!m_batchQueue.empty()) {
                BatchState s = m_batchQueue.front()->state.load();
                if (s == BatchState::Completed || s == BatchState::Failed || s == BatchState::Stopped)
                    m_batchQueue.pop_front();
                else break;
            }
        }

        if (st == BatchState::Failed && !batch->errorMessage.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "%s", batch->errorMessage.c_str());
        }

        // CRC results summary
        if (!batch->crcResults.empty()) {
            int pass = batch->crcPassCount();
            int fail = batch->crcFailCount();
            ImGui::Spacing();

            // Compute total CRC time across all files
            double totalCrcMs = 0, totalRemoteMs = 0, totalLocalMs = 0;
            for (auto& r : batch->crcResults) {
                totalCrcMs += r.totalMs;
                totalRemoteMs += r.remoteMs;
                totalLocalMs += r.localMs;
            }

            if (fail == 0) {
                ImGui::TextColored(ImVec4(0.3f,1,0.5f,1), "CRC verified: %d/%d files OK", pass, (int)batch->crcResults.size());
            } else {
                ImGui::TextColored(ImVec4(1,0.6f,0.2f,1), "CRC: %d passed, %d failed", pass, fail);
            }
            // Total CRC timing summary
            if (totalCrcMs > 0) {
                ImGui::TextDisabled("  CRC time: %.1fs total (device: %.1fs, local: %.1fs)",
                                    totalCrcMs / 1000.0, totalRemoteMs / 1000.0, totalLocalMs / 1000.0);
            }

            // Show details for failed CRCs (always), and passed (in a collapsible)
            if (fail > 0) {
                // Retry button at the top so it's immediately visible
                if (isDone && ImGui::Button("Retry Failed Files")) {
                    auto retryBatch = std::make_shared<TransferBatch>();
                    retryBatch->isPull = batch->isPull;
                    retryBatch->isLocalCopy = batch->isLocalCopy;
                    retryBatch->srcDeviceSlot = batch->srcDeviceSlot;
                    retryBatch->dstDeviceSlot = batch->dstDeviceSlot;
                    retryBatch->useParallelChannels = batch->useParallelChannels;
                    retryBatch->useMultiNic = batch->useMultiNic;
                    retryBatch->numChannels = batch->numChannels;
                    for (auto& r : batch->crcResults) {
                        if (r.passed || r.sourcePath.empty()) continue;
                        BatchFileItem item;
                        item.sourcePath = r.sourcePath;
                        item.destPath = r.destPath;
                        item.displayName = r.fileName;
                        item.fileSize = r.fileSize;
                        retryBatch->files.push_back(std::move(item));
                    }
                    if (!retryBatch->files.empty()) {
                        // Auto-overwrite for retries (the whole point is to re-copy)
                        retryBatch->conflictAllDecision = ConflictAction::OverwriteAll;
                        // Calculate totalBytes
                        uint64_t retryTotal = 0;
                        for (auto& f : retryBatch->files)
                            retryTotal += f.fileSize;
                        retryBatch->totalBytes = retryTotal;
                        {
                            std::lock_guard<std::mutex> lk(m_batchMutex);
                            m_batchQueue.push_back(retryBatch);
                        }
                        m_batchCV.notify_one();
                    }
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(re-copies failed files)");
                for (auto& r : batch->crcResults) {
                    if (r.passed) continue;
                    ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "  FAIL: %s", r.fileName.c_str());
                    ImGui::TextColored(ImVec4(0.7f,0.5f,0.5f,1), "        %s", r.detail.c_str());
                }
            }
            if (pass > 0 && ImGui::TreeNode("Passed files")) {
                for (auto& r : batch->crcResults) {
                    if (!r.passed) continue;
                    // Show timing per file
                    double throughputMBs = (r.fileSize > 0 && r.totalMs > 0)
                        ? (r.fileSize / (1024.0 * 1024.0)) / (r.totalMs / 1000.0) : 0;
                    ImGui::TextColored(ImVec4(0.5f,0.8f,0.5f,1), "  OK: %s", r.fileName.c_str());
                    ImGui::TextDisabled("      %s", r.detail.c_str());
                    if (r.totalMs > 0) {
                        ImGui::TextDisabled("      Device: %.0fms  Local: %.0fms  Total: %.0fms  (%.1f MB/s)",
                                            r.remoteMs, r.localMs, r.totalMs, throughputMBs);
                    }
                }
                ImGui::TreePop();
            }
        }
    }

    // Paused indicator
    if (isPaused) {
        ImGui::SameLine(0, 20);
        ImGui::TextColored(ImVec4(1,0.8f,0.2f,1), "PAUSED");
    }

    // Skipped files indicator (shown when completed)
    if (isDone && batch->skippedFiles > 0) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.4f, 1), "%d file(s) skipped (already existed)",
                           batch->skippedFiles);
    }

    // Error-skipped files summary
    if (isDone && batch->errorSkippedFiles.load() > 0) {
        ImGui::Spacing();
        int errSkipped = batch->errorSkippedFiles.load();
        ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "%d file(s) skipped due to errors", errSkipped);
        {
            std::lock_guard<std::mutex> lk(batch->errorSkippedMutex);
            if (!batch->errorSkippedNames.empty() && ImGui::TreeNode("Skipped files")) {
                for (auto& name : batch->errorSkippedNames)
                    ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "  %s", name.c_str());
                ImGui::TreePop();
            }
        }
    }

    // Track max width so the window only grows, never shrinks mid-transfer
    // Track max of window size and content width (whichever is larger)
    float curW = std::max(ImGui::GetWindowSize().x, ImGui::GetContentRegionAvail().x + ImGui::GetStyle().WindowPadding.x * 2);
    if (curW > trackedWidth) trackedWidth = curW;
    if (isDone) trackedWidth = 520.0f;

    ImGui::End();
    ImGui::PopStyleVar(); // alpha fade
}

static std::string buildLogText(const std::vector<LogEntry>& entries, const std::vector<int>& visible,
                                std::chrono::steady_clock::time_point startTime) {
    std::string text;
    text.reserve(visible.size() * 100);
    char timeBuf[32];
    for (int idx : visible) {
        auto& e = entries[idx];
        double secs = std::chrono::duration<double>(e.time - startTime).count();
        const char* lvl;
        switch (e.level) {
            case LogLevel::Debug: lvl = "DBG"; break;
            case LogLevel::Info:  lvl = "INF"; break;
            case LogLevel::Warn:  lvl = "WRN"; break;
            case LogLevel::Error: lvl = "ERR"; break;
            default:              lvl = "???"; break;
        }
        { auto t = std::chrono::system_clock::to_time_t(e.wallTime);
          auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(e.wallTime.time_since_epoch()).count() % 1000;
          struct tm tm; localtime_s(&tm, &t);
          snprintf(timeBuf, sizeof(timeBuf), "[%02d:%02d:%02d.%03d]", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)ms); }
        text += timeBuf;
        text += " ["; text += lvl; text += "] ["; text += e.tag; text += "] ";
        text += e.message;
        text += "\n";
    }
    return text;
}

void App::openAndroidFile(FilePanel& panel, int index) {
    if (!panel.isAndroid || !panel.validIndex(index)) return;
    if (panel.entryIsDir(index)) return;

    std::string name = panel.entryName(index);
    std::string remotePath = panel.currentPath;
    if (remotePath.back() != '/') remotePath += "/";
    remotePath += name;

    m_statusMessage = "Opening: " + name + "...";
    m_statusTime = std::chrono::steady_clock::now();

    int slot = panel.deviceSlot;
    postAsync("Opening: " + name, [this, remotePath, name, slot]() {
        char tempBuf[MAX_PATH];
        GetTempPathA(MAX_PATH, tempBuf);
        std::string tempDir = std::string(tempBuf) + "FastEnough\\";
        CreateDirectoryA(tempDir.c_str(), nullptr);

        std::string localPath = tempDir + name;
        uint64_t outSize = 0;

        LOG_INFO("UI", "Pulling to temp: " + remotePath + " -> " + localPath);
        bool ok = deviceForSlot(slot).pullFile(remotePath, localPath, outSize, nullptr);
        if (ok) {
            LOG_INFO("UI", "Opening: " + localPath);
            ShellExecuteA(nullptr, "open", localPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            m_statusMessage = "Opened: " + name;
        } else {
            LOG_ERROR("UI", "Failed to pull file: " + remotePath);
            m_statusMessage = "Failed to open: " + name;
        }
        m_statusTime = std::chrono::steady_clock::now();
    });
}

void App::renderCopyMoveDialog() {
    if (!m_showCopyMoveDialog || !m_pendingBatch) return;

    ImGui::OpenPopup("Copy or Move?");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Copy or Move?", &m_showCopyMoveDialog,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        int fileCount = (int)m_pendingBatch->files.size();
        if (fileCount == 1)
            ImGui::Text("%s", m_pendingBatch->files[0].displayName.c_str());
        else
            ImGui::Text("%d files selected", fileCount);

        ImGui::Spacing();

        // Check if same partition (for local copies — suggest move)
        bool sameDrive = false;
        if (m_pendingBatch->isLocalCopy && fileCount > 0) {
            auto& src = m_pendingBatch->files[0].sourcePath;
            auto& dst = m_pendingBatch->files[0].destPath;
            if (src.size() >= 2 && dst.size() >= 2)
                sameDrive = (toupper(src[0]) == toupper(dst[0]));
            if (sameDrive)
                ImGui::TextDisabled("Same drive — move will be instant");
        }

        ImGui::Spacing();

        float buttonW = 120.0f;
        float spacing = ImGui::GetStyle().ItemSpacing.x;
        float totalW = buttonW * 2 + spacing;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - totalW) * 0.5f);

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.35f, 0.55f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.45f, 0.70f, 1));
        if (ImGui::Button("Copy", ImVec2(buttonW, 30))) {
            m_pendingBatch->isMove = false;
            {
                std::lock_guard<std::mutex> lk(m_batchMutex);
                m_batchQueue.push_back(m_pendingBatch);
            }
            m_batchCV.notify_one();
            m_overlayWasOpen = false;
            m_pendingBatch.reset();
            m_showCopyMoveDialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(2);

        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.40f, 0.35f, 0.10f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.55f, 0.50f, 0.15f, 1));
        if (ImGui::Button("Move", ImVec2(buttonW, 30))) {
            m_pendingBatch->isMove = true;
            {
                std::lock_guard<std::mutex> lk(m_batchMutex);
                m_batchQueue.push_back(m_pendingBatch);
            }
            m_batchCV.notify_one();
            m_overlayWasOpen = false;
            m_pendingBatch.reset();
            m_showCopyMoveDialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(2);

        ImGui::EndPopup();
    }
}

void App::tryAutoWifiConnect(const std::string& serial) {
    if (m_wifiAutoSetupDone || m_dualChannelAvailable) return;
    if (!m_prefs.wifiAutoConnect) return; // user disabled WiFi auto-connect

    for (auto& w : m_prefs.savedWifiDevices) {
        if (w.serial == serial && w.autoConnect && !w.wifiIp.empty()) {
            // Check if WiFi serial is already in the device list (tcpip already active)
            std::string wifiSerial = w.wifiIp + ":" + std::to_string(w.port);
            bool alreadyInTcpip = false;
            {
                std::lock_guard<std::mutex> lk(m_deviceMutex);
                for (auto& d : m_devices)
                    if (d.serial == wifiSerial) { alreadyInTcpip = true; break; }
            }

            // Verify WiFi is actually reachable before disrupting USB with tcpip
            // Quick check: try connecting directly first (if tcpip was already set from a previous session)
            LOG_INFO("WiFi", "Auto-connecting to saved WiFi device: " + w.wifiIp + ":" + std::to_string(w.port) +
                     (alreadyInTcpip ? " (already in tcpip mode)" : ""));
            m_wifiAutoSetupDone = true;

            std::string ip = w.wifiIp;
            int port = w.port;
            postAsync("Connecting WiFi ADB...", [this, serial, ip, port, alreadyInTcpip]() {
                // First, check if the phone actually has WiFi by querying wlan0
                if (!alreadyInTcpip) {
                    std::string wlanCheck = m_device.runAdbCommand("-s " + serial + " shell \"ip -4 addr show wlan0 2>/dev/null\"");
                    if (wlanCheck.find("inet ") == std::string::npos) {
                        LOG_INFO("WiFi", "WiFi not available on device - skipping auto-connect");
                        m_statusMessage = "WiFi not available - using USB only";
                        m_statusTime = std::chrono::steady_clock::now();
                        return;
                    }
                    // Enable tcpip mode (causes temporary USB disconnect)
                    m_device.runAdbCommand("-s " + serial + " tcpip " + std::to_string(port));
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                }
                // Connect
                std::string result = m_device.runAdbCommand("connect " + ip + ":" + std::to_string(port));
                if (result.find("connected") != std::string::npos || result.find("already") != std::string::npos) {
                    LOG_INFO("WiFi", "WiFi ADB connected: " + ip);
                    // Track USB↔WiFi serial mapping
                    m_usbToWifiSerial = ip + ":" + std::to_string(port);
                    m_wifiToUsbSerial = serial;
                    // Establish secondary channel
                    m_secondaryChannel.setAdbPath(m_device.getAdbPath());
                    if (m_secondaryChannel.connectTcp(ip, AFM_PORT) && m_secondaryChannel.verifyConnection()) {
                        m_dualChannelAvailable = true;
                        m_secondaryChannelType = "WiFi";
                        m_statusMessage = "Dual channel active: USB + WiFi";
                        LOG_INFO("WiFi", "Dual channel established via auto-connect");
                    } else {
                        m_statusMessage = "WiFi connected but dual channel failed";
                    }
                } else {
                    LOG_WARN("WiFi", "WiFi auto-connect failed: " + result);
                }
                m_statusTime = std::chrono::steady_clock::now();
            });
            return;
        }
    }
}

void App::renderWifiBanner() {
    return; // Dual channel is now automatic via USB x2 / WiFi x2 toggles
    if (m_wifiBannerDismissed || m_wifiAutoSetupDone || m_dualChannelAvailable) return;
    if (!m_device.isServerRunning()) return;

    // Check if USB device has WiFi and isn't already saved
    std::string serial = m_device.connectedSerial();
    if (serial.empty()) return;
    for (auto& w : m_prefs.savedWifiDevices)
        if (w.serial == serial) return; // already saved

    // Detect WiFi IP (check once, not every frame)
    if (!m_wifiBannerShown) {
        // Rate-limit: only check every 10 seconds
        static auto lastCheck = std::chrono::steady_clock::time_point{};
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - lastCheck).count() < 10.0) return;
        lastCheck = now;

        // Run on async thread to avoid blocking UI
        postAsync("", [this, serial]() {
            std::string wlanOut = m_device.runAdbCommand("-s " + serial + " shell \"ip -4 addr show wlan0 2>/dev/null\"");
            if (wlanOut.find("inet ") != std::string::npos)
                m_wifiBannerShown = true;
        });
        return;
    }

    // Show banner at top of main window
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.15f, 0.25f, 1));
    ImGui::BeginChild("##WifiBanner", ImVec2(0, ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.y * 2), ImGuiChildFlags_Border);
    float pad = (ImGui::GetContentRegionAvail().y - ImGui::GetFontSize()) * 0.5f;
    ImGui::SetCursorPosY(pad);
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1, 1), "  WiFi detected on your phone.");
    ImGui::SameLine();
    ImGui::TextDisabled("Enable dual-channel for faster transfers?");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.35f, 0.55f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.45f, 0.70f, 1));
    if (ImGui::SmallButton("Set Up")) {
        m_showWifiWizard = true;
        m_wizardStep = 0;
    }
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    if (ImGui::SmallButton("Dismiss")) {
        m_wifiBannerDismissed = true;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void App::renderWifiWizard() {
    if (!m_showWifiWizard) return;

    ImGui::OpenPopup("WiFi Setup Wizard");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("WiFi Setup Wizard", &m_showWifiWizard,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {

        if (m_wizardStep == 0) {
            // Step 1: Select device
            ImGui::TextColored(ImVec4(0.45f, 0.70f, 1, 1), "Step 1: Select Device");
            ImGui::Separator();
            ImGui::Spacing();

            // List available devices
            std::vector<DeviceInfo> devSnap;
            {
                std::lock_guard<std::mutex> lk(m_deviceMutex);
                devSnap = m_devices;
            }

            int onlineCount = 0;
            for (auto& d : devSnap) if (d.state == "device") onlineCount++;

            if (onlineCount > 0) {
                ImGui::TextDisabled("Select the device to set up WiFi ADB:");
                ImGui::Spacing();

                for (int i = 0; i < (int)devSnap.size(); i++) {
                    if (devSnap[i].state != "device") continue;
                    bool isWifi = isWifiSerial(devSnap[i].serial);

                    std::string label = devSnap[i].serial;
                    if (!devSnap[i].model.empty()) label = devSnap[i].model + " (" + devSnap[i].serial + ")";
                    if (isWifi) label += "  [WiFi]";
                    else label += "  [USB]";

                    bool selected = (m_wizardSerial == devSnap[i].serial);
                    if (ImGui::Selectable(label.c_str(), selected)) {
                        m_wizardSerial = devSnap[i].serial;
                    }
                }

                ImGui::Spacing();
                if (!m_wizardSerial.empty()) {
                    if (ImGui::Button("Next", ImVec2(120, 0))) m_wizardStep = 1;
                } else {
                    ImGui::TextDisabled("Click a device above to select it.");
                }
            } else {
                ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "No device detected.");
                ImGui::Spacing();
                ImGui::TextDisabled("1. Connect your phone with a USB cable");
                ImGui::Spacing();
                ImGui::TextDisabled("2. On your phone, go to Settings > Developer Options");
                ImGui::TextDisabled("   and enable USB Debugging");
                ImGui::Spacing();
                ImGui::TextDisabled("3. If this is your first time connecting, a pairing");
                ImGui::TextDisabled("   prompt will appear on your phone - tap Allow");
                ImGui::Spacing();
                ImGui::TextDisabled("If you don't see Developer Options, go to");
                ImGui::TextDisabled("Settings > About Phone and tap Build Number 7 times.");
            }
        } else if (m_wizardStep == 1) {
            // Step 2: Check WiFi
            ImGui::TextColored(ImVec4(0.45f, 0.70f, 1, 1), "Step 2: WiFi Connection");
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextDisabled("Device: %s", m_wizardSerial.c_str());
            ImGui::Spacing();

            std::string serial = m_wizardSerial;
            std::string wlanOut = m_device.runAdbCommand("-s " + serial + " shell \"ip -4 addr show wlan0 2>/dev/null\"");
            m_wizardWifiIp.clear();
            auto inetPos = wlanOut.find("inet ");
            if (inetPos != std::string::npos) {
                auto start = inetPos + 5;
                auto slash = wlanOut.find('/', start);
                if (slash != std::string::npos)
                    m_wizardWifiIp = wlanOut.substr(start, slash - start);
            }

            if (!m_wizardWifiIp.empty()) {
                ImGui::TextColored(ImVec4(0.3f, 1, 0.5f, 1), "WiFi connected: %s", m_wizardWifiIp.c_str());
                ImGui::Spacing();
                if (ImGui::Button("Set Up Wireless Connection", ImVec2(250, 0))) {
                    m_wizardStep = 2;
                    m_wizardBusy = true;
                    m_wizardStatus = "Enabling WiFi ADB...";
                    std::string ip = m_wizardWifiIp;
                    postAsync("Setting up WiFi ADB...", [this, serial, ip]() {
                        // Enable tcpip on port 5555
                        m_wizardStatus = "Running adb tcpip 5555...";
                        m_device.runAdbCommand("-s " + serial + " tcpip 5555");
                        std::this_thread::sleep_for(std::chrono::seconds(2));

                        // Connect via WiFi
                        m_wizardStatus = "Connecting to " + ip + ":5555...";
                        std::string result = m_device.runAdbCommand("connect " + ip + ":5555");
                        bool ok = (result.find("connected") != std::string::npos ||
                                   result.find("already") != std::string::npos);

                        if (ok) {
                            // Save for auto-reconnect
                            SavedWifiDevice saved;
                            saved.serial = serial;
                            saved.wifiIp = ip;
                            saved.port = 5555;
                            saved.autoConnect = true;
                            // Query display name (marketing name preferred)
                            saved.model = queryDeviceDisplayName(serial);
                            // Track USB↔WiFi serial mapping
                            m_usbToWifiSerial = ip + ":5555";
                            m_wifiToUsbSerial = serial;
                            // Remove old entry for same serial
                            auto& devices = m_prefs.savedWifiDevices;
                            devices.erase(std::remove_if(devices.begin(), devices.end(),
                                [&](auto& d) { return d.serial == serial; }), devices.end());
                            devices.push_back(saved);
                            m_prefs.wifiAutoConnect = true; // enable auto-connect after successful wizard
                            m_prefs.save();

                            // Try dual channel
                            std::this_thread::sleep_for(std::chrono::seconds(1));
                            m_secondaryChannel.setAdbPath(m_device.getAdbPath());
                            if (m_secondaryChannel.connectTcp(ip, AFM_PORT) && m_secondaryChannel.verifyConnection()) {
                                m_dualChannelAvailable = true;
                                m_secondaryChannelType = "WiFi";
                                m_wizardStatus = "Dual channel active!";
                            } else {
                                m_wizardStatus = "WiFi connected (dual channel will activate on next transfer)";
                            }
                            m_wifiAutoSetupDone = true;
                        } else {
                            m_wizardStatus = "Failed: " + result;
                        }
                        m_wizardBusy = false;
                        m_wizardStep = 3;
                    });
                }
            } else {
                ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "Phone is not connected to WiFi.");
                ImGui::TextDisabled("Connect your phone to a WiFi network,\nthen click Check Again.");
                ImGui::Spacing();
                if (ImGui::Button("Check Again", ImVec2(120, 0))) {} // re-renders, re-checks
            }
            ImGui::SameLine();
            if (ImGui::Button("Back", ImVec2(80, 0))) m_wizardStep = 0;
        } else if (m_wizardStep == 2) {
            // Step 3: Setting up
            ImGui::TextColored(ImVec4(0.45f, 0.70f, 1, 1), "Step 3: Setting Up");
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", m_wizardStatus.c_str());
            // Animated dots
            float t = (float)fmod(ImGui::GetTime(), 1.0);
            int dots = (int)(t * 4) % 4;
            ImGui::SameLine();
            ImGui::Text("%.*s", dots, "...");
        } else if (m_wizardStep == 3) {
            // Step 4: Done
            ImGui::TextColored(ImVec4(0.45f, 0.70f, 1, 1), "Setup Complete");
            ImGui::Separator();
            ImGui::Spacing();

            if (m_dualChannelAvailable) {
                ImGui::TextColored(ImVec4(0.3f, 1, 0.5f, 1), "Dual-channel is active!");
                ImGui::Spacing();
                ImGui::TextDisabled("Transfers will now use both USB and WiFi simultaneously.\n"
                                    "This device will auto-connect via WiFi on future launches.\n"
                                    "You can unplug USB and WiFi will keep working.");
            } else if (m_wizardStatus.find("Failed") != std::string::npos) {
                ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "%s", m_wizardStatus.c_str());
            } else {
                ImGui::TextColored(ImVec4(0.3f, 1, 0.5f, 1), "%s", m_wizardStatus.c_str());
            }

            ImGui::Spacing();
            if (ImGui::Button("Close", ImVec2(120, 0))) {
                m_showWifiWizard = false;
                m_wifiBannerDismissed = true;
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::EndPopup();
    }
}

void App::renderWifiPairingDialog() {
    if (!m_showWifiPairing) return;

    ImGui::OpenPopup("WiFi ADB Pairing");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("WiFi ADB Pairing", &m_showWifiPairing,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextColored(ImVec4(0.45f, 0.70f, 1, 1), "Android 11+ Wireless Debugging");
        ImGui::Separator();
        ImGui::Spacing();

        if (!m_pairingDone) {
            // Step 1: Pair
            ImGui::TextDisabled("On your phone: Settings > Developer Options > Wireless Debugging");
            ImGui::TextDisabled("Tap 'Pair device with pairing code' and enter the details below.");
            ImGui::Spacing();

            ImGui::SetNextItemWidth(250);
            ImGui::InputTextWithHint("Pairing IP:Port", "e.g. 192.168.1.100:37000", m_pairingIp, sizeof(m_pairingIp));
            ImGui::SetNextItemWidth(250);
            ImGui::InputTextWithHint("Pairing Code", "e.g. 123456", m_pairingCode, sizeof(m_pairingCode));
            ImGui::Spacing();

            if (ImGui::Button("Pair", ImVec2(120, 0))) {
                std::string ip(m_pairingIp);
                std::string code(m_pairingCode);
                postAsync("Pairing...", [this, ip, code]() {
                    std::string result = m_device.runAdbCommand("pair " + ip + " " + code);
                    if (result.find("Successfully") != std::string::npos) {
                        m_pairingDone = true;
                        m_statusMessage = "Paired! Enter the connect IP:port from Wireless Debugging.";
                    } else {
                        m_statusMessage = "Pairing failed: " + result;
                    }
                    m_statusTime = std::chrono::steady_clock::now();
                });
            }
        } else {
            // Step 2: Connect (after successful pairing)
            ImGui::TextColored(ImVec4(0.3f, 1, 0.5f, 1), "Paired successfully!");
            ImGui::Spacing();
            ImGui::TextDisabled("Now enter the connect IP:port shown at the top of");
            ImGui::TextDisabled("the Wireless Debugging screen (NOT the pairing port).");
            ImGui::Spacing();

            ImGui::SetNextItemWidth(250);
            ImGui::InputTextWithHint("Connect IP:Port", "e.g. 192.168.1.100:42135", m_connectIp, sizeof(m_connectIp));
            ImGui::Spacing();

            if (ImGui::Button("Connect", ImVec2(120, 0))) {
                std::string connectAddr(m_connectIp);
                postAsync("Connecting...", [this, connectAddr]() {
                    std::string result = m_device.runAdbCommand("connect " + connectAddr);
                    if (result.find("connected") != std::string::npos || result.find("already") != std::string::npos) {
                        // Parse IP and port from the connect address
                        std::string ip = connectAddr;
                        int port = 5555;
                        auto colon = connectAddr.rfind(':');
                        if (colon != std::string::npos) {
                            ip = connectAddr.substr(0, colon);
                            try { port = std::stoi(connectAddr.substr(colon + 1)); } catch (...) {}
                        }
                        // Save for auto-reconnect
                        SavedWifiDevice saved;
                        saved.serial = connectAddr; // WiFi serial IS the connect address
                        saved.wifiIp = ip;
                        saved.port = port;
                        saved.autoConnect = true;
                        // Query display name from newly connected device
                        saved.model = queryDeviceDisplayName(connectAddr);
                        m_prefs.savedWifiDevices.push_back(saved);
                        m_prefs.save();
                        m_statusMessage = "Connected via WiFi: " + connectAddr;
                    } else {
                        m_statusMessage = "Connect failed: " + result;
                    }
                    m_statusTime = std::chrono::steady_clock::now();
                });
                m_showWifiPairing = false;
                m_pairingDone = false;
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80, 0))) {
            m_showWifiPairing = false;
            m_pairingDone = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void App::renderCrossDeviceDialog() {
    if (!m_showCrossDeviceDialog || !m_pendingBatch) return;

    ImGui::OpenPopup("Cross-Device Transfer");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Cross-Device Transfer", &m_showCrossDeviceDialog,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        int fileCount = (int)m_pendingBatch->files.size();
        ImGui::Text("Transfer %d file(s) between devices", fileCount);
        ImGui::Spacing();

        ImGui::TextColored(ImVec4(0.45f, 0.70f, 1, 1), "Choose transfer method:");
        ImGui::Spacing();

        bool dokanAvailable = m_pendingBatch &&
            DeviceMountManager::instance(m_pendingBatch->srcDeviceSlot).isMounted();

        // Dokan streaming option
        ImGui::BeginDisabled(!dokanAvailable);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.35f, 0.55f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.45f, 0.70f, 1));
        if (ImGui::Button("Stream via Dokan", ImVec2(250, 0))) {
            m_pendingBatch->useDokanRelay = true;
            {
                std::lock_guard<std::mutex> lk(m_batchMutex);
                m_batchQueue.push_back(m_pendingBatch);
            }
            m_batchCV.notify_one();
            m_overlayWasOpen = false;
            m_pendingBatch.reset();
            m_showCrossDeviceDialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::EndDisabled();
        if (dokanAvailable)
            ImGui::TextDisabled("  Streams directly from device A to B through PC.\n  No temp files. Requires Dokan mount.");
        else
            ImGui::TextDisabled("  Not available — mount device A via Dokan first\n  (Connection > Mount as Drive).");

        ImGui::Spacing();

        // Temp relay option
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.40f, 0.35f, 0.10f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.55f, 0.50f, 0.15f, 1));
        if (ImGui::Button("Relay via PC (temp file)", ImVec2(250, 0))) {
            m_pendingBatch->useDokanRelay = false;
            {
                std::lock_guard<std::mutex> lk(m_batchMutex);
                m_batchQueue.push_back(m_pendingBatch);
            }
            m_batchCV.notify_one();
            m_overlayWasOpen = false;
            m_pendingBatch.reset();
            m_showCrossDeviceDialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::TextDisabled("  Pulls from device A to PC temp folder,\n  then pushes to device B. Always works.");

        ImGui::EndPopup();
    }
}

void App::renderAboutPopup() {
    if (m_showAboutPopup) {
        ImGui::OpenPopup("About##AboutPopup");
        m_showAboutPopup = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("About##AboutPopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.70f, 1.00f, 1.00f));
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize("Fast Enough? - Android File Explorer").x) * 0.5f);
        ImGui::Text("Fast Enough? - Android File Explorer");
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Version: 1.0.12");
        ImGui::Text("Build date: %s", __DATE__);
        ImGui::Spacing();
        ImGui::Text("Made by: JohnTheFarmer");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        float buttonWidth = 120.0f;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - buttonWidth) * 0.5f);
        if (ImGui::Button("Close", ImVec2(buttonWidth, 0))) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void App::renderNotificationPopup() {
    if (m_showNotification) {
        ImGui::OpenPopup("##NotificationPopup");
        m_showNotification = false;
    }
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(420, 0));
    if (ImGui::BeginPopupModal("##NotificationPopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::Spacing();
        ImVec4 titleColor = m_notificationIsError ? ImVec4(1, 0.4f, 0.4f, 1) : ImVec4(0.3f, 1, 0.5f, 1);
        ImGui::TextColored(titleColor, "%s", m_notificationTitle.c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextWrapped("%s", m_notificationMessage.c_str());
        ImGui::Spacing();
        ImGui::Spacing();
        float btnW = 80;
        ImGui::SetCursorPosX((ImGui::GetWindowSize().x - btnW) * 0.5f);
        if (ImGui::Button("OK", ImVec2(btnW, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::Spacing();
        ImGui::EndPopup();
    }
}

void App::renderCloseConfirmDialog() {
    if (m_showCloseConfirm) {
        ImGui::OpenPopup("Close Application?");
        m_showCloseConfirm = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Close Application?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::Text("Are you sure you want to close the application?");
        ImGui::Spacing();
        ImGui::Spacing();

        float buttonWidth = 120.0f;
        float spacing = ImGui::GetStyle().ItemSpacing.x;
        float totalWidth = buttonWidth * 2 + spacing;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - totalWidth) * 0.5f);

        if (ImGui::Button("Yes", ImVec2(buttonWidth, 0))) {
            g_reallyQuit = true;
            PostMessage(g_mainHwnd, WM_CLOSE, 0, 0);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("No", ImVec2(buttonWidth, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void App::renderPreferencesWindow() {
    ImGui::SetNextWindowSize(ImVec2(450, 0), ImGuiCond_Appearing);
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (!ImGui::Begin("Preferences", &m_showPreferences, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    bool changed = false;

    // --- ADB ---
    ImGui::TextColored(ImVec4(0.45f,0.70f,1,1), "ADB");
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Checkbox("Restart ADB daemon on app launch", &m_prefs.restartAdbOnLaunch))
        changed = true;
    ImGui::TextDisabled("  Kills and restarts ADB when the app starts.");
    ImGui::TextDisabled("  Useful if ADB is in a bad state, but disrupts");
    ImGui::TextDisabled("  other ADB clients (Android Studio, scrcpy, etc.).");

    ImGui::Spacing();

    if (ImGui::Checkbox("Kill ADB server when app closes", &m_prefs.killAdbOnClose))
        changed = true;
    ImGui::TextDisabled("  Stops the ADB daemon when exiting the app.");
    ImGui::TextDisabled("  Disable if you want ADB to stay running for");
    ImGui::TextDisabled("  other tools (Android Studio, scrcpy, etc.).");

    ImGui::Spacing();
    ImGui::Spacing();

    // --- General ---
    ImGui::TextColored(ImVec4(0.45f,0.70f,1,1), "General");
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Checkbox("Ask for confirmation before closing", &m_prefs.confirmOnClose))
        changed = true;
    ImGui::TextDisabled("  Shows a Yes/No dialog when pressing the X button.");
    ImGui::TextDisabled("  When disabled, closing minimizes to the system tray.");

    ImGui::Spacing();
    ImGui::Spacing();

    // --- Transfer ---
    ImGui::TextColored(ImVec4(0.45f,0.70f,1,1), "Transfer");
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Checkbox("Verify file integrity (CRC32) after transfer", &m_prefs.enableCrcVerification))
        changed = true;
    ImGui::TextDisabled("  Computes CRC32 on both sides after each file");
    ImGui::TextDisabled("  transfer to ensure data integrity.");

    ImGui::Spacing();

    if (ImGui::Checkbox("Auto-dismiss transfer window on success", &m_prefs.autoDismissTransfer))
        changed = true;
    ImGui::TextDisabled("  Automatically closes the transfer progress window");
    ImGui::TextDisabled("  when all files complete successfully.");

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(1,0.85f,0.4f,1), "Transfer Channels");
    ImGui::Spacing();

    {
        bool usbDual = (m_prefs.usbPipeCount == 2);
        if (ImGui::Checkbox("USB dual-pipe (2x USB throughput)", &usbDual)) {
            m_prefs.usbPipeCount = usbDual ? 2 : 1;
            changed = true;
        }
        ImGui::TextDisabled("  Two parallel ADB connections through USB.");
        ImGui::TextDisabled("  Best for USB 3.x. Disable for USB 2.0 devices.");
    }

    ImGui::Spacing();

    {
        bool wifiDual = (m_prefs.wifiPipeCount == 2);
        if (ImGui::Checkbox("WiFi dual-pipe (2x WiFi throughput)", &wifiDual)) {
            m_prefs.wifiPipeCount = wifiDual ? 2 : 1;
            changed = true;
        }
        ImGui::TextDisabled("  Two parallel TCP connections over WiFi.");
        ImGui::TextDisabled("  Best for fast WiFi (WiFi 6/7).");
    }

    ImGui::Spacing();
    ImGui::TextDisabled("  Changes apply on next device connect.");
    ImGui::TextDisabled("  USB + WiFi dual = up to 4 parallel channels.");

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (changed) {
        m_prefs.save();
        m_statusMessage = "Preferences saved";
        m_statusTime = std::chrono::steady_clock::now();
    }

    float bw = 120.0f;
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - bw) * 0.5f);
    if (ImGui::Button("Close", ImVec2(bw, 0))) {
        m_showPreferences = false;
    }

    ImGui::End();
}

void App::renderNicConfigWindow() {
    float minW = 580.0f * (m_systemDpiScale * m_theme.userScale);
    ImGui::SetNextWindowSizeConstraints(ImVec2(minW, 0), ImVec2(FLT_MAX, FLT_MAX));
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (!ImGui::Begin("Multi-NIC Configuration", &m_showNicConfig,
                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    bool changed = false;

    ImGui::TextColored(ImVec4(0.45f,0.70f,1,1), "Multi-NIC Parallel Transfers");
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Checkbox("Enable multi-NIC parallel transfers", &m_prefs.enableMultiNic))
        changed = true;
    ImGui::TextDisabled("  Uses multiple network adapters simultaneously for");
    ImGui::TextDisabled("  faster file transfers. Requires direct TCP connection.");
    ImGui::TextDisabled("  Each NIC carries a portion of the data independently.");

    ImGui::Spacing();
    ImGui::Spacing();

    // Scan button
    ImGui::TextColored(ImVec4(0.45f,0.70f,1,1), "Network Adapters");
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Scan Network Adapters")) {
        m_detectedNics = enumerateNics();
        m_nicTestResults.assign(m_detectedNics.size(), false);
        m_nicTestDone.assign(m_detectedNics.size(), false);
        m_nicTestReason.assign(m_detectedNics.size(), "");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%d found)", (int)m_detectedNics.size());

    ImGui::Spacing();

    if (!m_detectedNics.empty()) {
        if (ImGui::BeginTable("NICs", 5,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Use", ImGuiTableColumnFlags_WidthFixed, 35);
            ImGui::TableSetupColumn("Adapter", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("IP", ImGuiTableColumnFlags_WidthFixed, 120);
            ImGui::TableSetupColumn("Speed", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Test", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableHeadersRow();

            for (int i = 0; i < (int)m_detectedNics.size(); i++) {
                auto& nic = m_detectedNics[i];
                ImGui::PushID(i);
                ImGui::TableNextRow();

                // Checkbox: find or create binding for this NIC
                ImGui::TableSetColumnIndex(0);
                bool isEnabled = false;
                int bindIdx = -1;
                for (int b = 0; b < (int)m_prefs.multiNicBindings.size(); b++) {
                    if (m_prefs.multiNicBindings[b].localIp == nic.ipAddress) {
                        isEnabled = m_prefs.multiNicBindings[b].enabled;
                        bindIdx = b;
                        break;
                    }
                }
                if (ImGui::Checkbox("##en", &isEnabled)) {
                    if (bindIdx >= 0) {
                        m_prefs.multiNicBindings[bindIdx].enabled = isEnabled;
                    } else {
                        NicBinding nb;
                        nb.adapterName = nic.adapterName;
                        nb.localIp = nic.ipAddress;
                        nb.enabled = isEnabled;
                        m_prefs.multiNicBindings.push_back(std::move(nb));
                    }
                    changed = true;
                }

                // Adapter name
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(nic.adapterName.c_str());
                if (ImGui::IsItemHovered() && !nic.description.empty())
                    ImGui::SetTooltip("%s", nic.description.c_str());

                // IP
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(nic.ipAddress.c_str());

                // Speed
                ImGui::TableSetColumnIndex(3);
                if (nic.speed >= 1000000000ULL)
                    ImGui::Text("%.0f Gbps", nic.speed / 1e9);
                else if (nic.speed >= 1000000ULL)
                    ImGui::Text("%.0f Mbps", nic.speed / 1e6);
                else if (nic.speed > 0)
                    ImGui::Text("%llu bps", nic.speed);
                else
                    ImGui::TextDisabled("?");

                // Test button: changes color to show result
                ImGui::TableSetColumnIndex(4);
                bool tested = i < (int)m_nicTestDone.size() && m_nicTestDone[i];
                bool passed = tested && m_nicTestResults[i];
                if (tested) {
                    if (passed) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.45f, 0.20f, 1));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.55f, 0.28f, 1));
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.15f, 0.15f, 1));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65f, 0.20f, 0.20f, 1));
                    }
                }
                const char* btnLabel = tested ? (passed ? " OK " : "Fail") : "Test";
                if (ImGui::SmallButton(btnLabel)) {
                    // Try connecting to the current device through this NIC
                    bool ok = false;
                    std::string reason;
                    if (!m_device.isServerRunning()) {
                        reason = "No device connected";
                    } else {
                        // Find the device's WiFi IP from any available source
                        std::string devIp = m_device.deviceIp();
                        if (devIp.empty() || !m_device.isDirectConnection()) {
                            // Try ADB to get wlan0 IP
                            std::string serial = m_device.connectedSerial();
                            std::string wlanOut = m_device.runAdbCommand(
                                "-s " + serial + " shell \"ip -4 addr show wlan0 2>/dev/null\"");
                            auto inetPos = wlanOut.find("inet ");
                            if (inetPos != std::string::npos) {
                                auto start = inetPos + 5;
                                auto slash = wlanOut.find('/', start);
                                if (slash != std::string::npos)
                                    devIp = wlanOut.substr(start, slash - start);
                            }
                            // Fallback: saved WiFi devices
                            if (devIp.empty()) {
                                for (auto& w : m_prefs.savedWifiDevices) {
                                    if (w.serial == serial && !w.wifiIp.empty()) {
                                        devIp = w.wifiIp;
                                        break;
                                    }
                                }
                            }
                        }
                        if (devIp.empty()) {
                            reason = "Could not determine device WiFi IP";
                        } else {
                            DeviceClient testCh;
                            if (!testCh.connectTcp(devIp, AFM_PORT, nic.ipAddress)) {
                                reason = "Could not reach " + devIp + " through " + nic.adapterName + " (" + nic.ipAddress + ")";
                            } else if (!testCh.verifyConnection()) {
                                reason = "Connected but server did not respond";
                                testCh.disconnectTcp();
                            } else {
                                ok = true;
                                testCh.disconnectTcp();
                            }
                        }
                    }
                    if (i < (int)m_nicTestResults.size()) {
                        m_nicTestResults[i] = ok;
                        m_nicTestDone[i] = true;
                        m_nicTestReason[i] = reason;
                    }
                }
                if (tested) ImGui::PopStyleColor(2);
                if (tested && !passed && ImGui::IsItemHovered()) {
                    std::string reason = (i < (int)m_nicTestReason.size()) ? m_nicTestReason[i] : "";
                    if (!reason.empty())
                        ImGui::SetTooltip("%s", reason.c_str());
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // Count enabled NICs
        int enabledCount = 0;
        for (auto& nb : m_prefs.multiNicBindings)
            if (nb.enabled) enabledCount++;
        ImGui::Spacing();
        if (enabledCount >= 2)
            ImGui::TextColored(ImVec4(0.3f,0.9f,0.3f,1), "%d adapters selected for parallel transfers", enabledCount);
        else if (enabledCount == 1)
            ImGui::TextColored(ImVec4(0.9f,0.7f,0.2f,1), "Select at least 2 adapters for parallel transfers");
        else
            ImGui::TextDisabled("No adapters selected");
    }

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (changed) {
        m_prefs.save();
        m_statusMessage = "Multi-NIC settings saved";
        m_statusTime = std::chrono::steady_clock::now();
    }

    float bw = 120.0f;
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - bw) * 0.5f);
    if (ImGui::Button("Close", ImVec2(bw, 0))) {
        m_showNicConfig = false;
    }

    ImGui::End();
}

void App::applyScale(float newScale) {
    m_theme.userScale = newScale;

    // Rebuild font at new size
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    float effectiveScale = m_systemDpiScale * newScale;
    float fontSize = 16.0f * effectiveScale;

    ImFontConfig fontCfg;
    fontCfg.OversampleH = 3;
    fontCfg.OversampleV = 3;
    fontCfg.PixelSnapH = true;
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

    // Tell DX11 backend to rebuild font texture
    extern void ImGui_ImplDX11_InvalidateDeviceObjects();
    ImGui_ImplDX11_InvalidateDeviceObjects();

    // Save current colors before resetting style
    ImVec4 savedColors[ImGuiCol_COUNT];
    if (m_theme.customColors) {
        for (int i = 0; i < ImGuiCol_COUNT; i++)
            savedColors[i] = m_theme.colors[i];
    }

    // Reset style to base sizes, then apply setupStyle (sets both sizes and colors)
    setupStyle();
    // Scale all sizes to effective DPI * user scale
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(effectiveScale);
    // Clamp WindowMinSize to avoid ImGui assertion at low effective scales
    if (style.WindowMinSize.x < 1.0f) style.WindowMinSize.x = 1.0f;
    if (style.WindowMinSize.y < 1.0f) style.WindowMinSize.y = 1.0f;

    // Restore custom colors if active (setupStyle overwrote them)
    if (m_theme.customColors) {
        ImVec4* c = ImGui::GetStyle().Colors;
        for (int i = 0; i < ImGuiCol_COUNT; i++)
            c[i] = savedColors[i];
    }
}

void App::renderThemeWindow() {
    // After a scale change, force re-center and re-size so the window doesn't end up off-screen
    ImGuiCond posCond = ImGuiCond_Appearing;
    ImGuiCond sizeCond = ImGuiCond_FirstUseEver;
    if (m_themeNeedsReposition) {
        posCond = ImGuiCond_Always;
        sizeCond = ImGuiCond_Always;
        m_themeNeedsReposition = false;
    }
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImVec2 vpSize = ImGui::GetMainViewport()->Size;
    float winH = std::min(600.0f, vpSize.y * 0.85f);
    ImGui::SetNextWindowSize(ImVec2(500, winH), sizeCond);
    ImGui::SetNextWindowSizeConstraints(ImVec2(400, 300), ImVec2(vpSize.x * 0.9f, vpSize.y * 0.9f));
    ImGui::SetNextWindowPos(center, posCond, ImVec2(0.5f, 0.5f));

    if (!ImGui::Begin("Appearance", &m_showThemeWindow, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    bool changed = false;

    // --- UI Scale ---
    ImGui::TextColored(ImVec4(0.45f, 0.70f, 1, 1), "UI Scale");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextDisabled("System DPI: %.0f%%", m_systemDpiScale * 100.0f);

    if (m_scalePreview < 0) m_scalePreview = m_theme.userScale;

    // Discrete 5% steps: index 0=50%, 1=55%, ..., 50=300%
    int step = ((int)(m_scalePreview * 100.0f + 0.5f) - 50) / 5;
    if (step < 0) step = 0; if (step > 50) step = 50;
    ImGui::SetNextItemWidth(250);
    char stepLabel[16]; snprintf(stepLabel, sizeof(stepLabel), "%d%%%%", step * 5 + 50);
    if (ImGui::SliderInt("App Scale", &step, 0, 50, stepLabel)) {
        m_scalePreview = (step * 5 + 50) / 100.0f;
    }

    int scalePct = step * 5 + 50;
    int currentPct = ((int)(m_theme.userScale * 100.0f + 0.5f) / 5) * 5;
    bool scaleChanged = (scalePct != currentPct);

    ImGui::SameLine();
    if (scaleChanged) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.35f, 0.55f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.45f, 0.70f, 1));
        if (ImGui::Button("Apply")) {
            m_pendingScale = m_scalePreview;
            m_themeNeedsReposition = true;
            changed = true;
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
    }
    if (ImGui::Button("Reset##scale")) {
        m_scalePreview = 1.0f;
        if (m_theme.userScale != 1.0f) {
            m_pendingScale = 1.0f;
            m_themeNeedsReposition = true;
            changed = true;
        }
    }
    ImGui::TextDisabled("  Current: %.0f%%  %s", m_systemDpiScale * m_theme.userScale * 100.0f,
                        scaleChanged ? "(unsaved)" : "");

    ImGui::Spacing();
    ImGui::Spacing();

    // --- Colors ---
    ImGui::TextColored(ImVec4(0.45f, 0.70f, 1, 1), "Colors");
    ImGui::Separator();
    ImGui::Spacing();

    if (!m_theme.customColors) {
        ImGui::TextDisabled("Using built-in OLED Black theme.");
        ImGui::Spacing();
        if (ImGui::Button("Customize Colors")) {
            // Copy current colors into theme for editing
            ImVec4* c = ImGui::GetStyle().Colors;
            for (int i = 0; i < ImGuiCol_COUNT; i++)
                m_theme.colors[i] = c[i];
            m_theme.customColors = true;
            changed = true;
        }
    } else {
        if (ImGui::Button("Reset to Default Theme")) {
            m_theme.customColors = false;
            setupStyle();
            // Re-apply scale
            ImGui::GetStyle().ScaleAllSizes(m_systemDpiScale * m_theme.userScale);
            if (ImGui::GetStyle().WindowMinSize.x < 1.0f) ImGui::GetStyle().WindowMinSize.x = 1.0f;
            if (ImGui::GetStyle().WindowMinSize.y < 1.0f) ImGui::GetStyle().WindowMinSize.y = 1.0f;
            changed = true;
        }

        ImGui::Spacing();

        // Color editor — grouped by category
        ImVec4* c = ImGui::GetStyle().Colors;

        struct ColorGroup {
            const char* name;
            std::vector<std::pair<int, const char*>> entries;
        };

        ColorGroup groups[] = {
            {"Backgrounds", {
                {ImGuiCol_WindowBg, "Window"},
                {ImGuiCol_ChildBg, "Child"},
                {ImGuiCol_PopupBg, "Popup"},
                {ImGuiCol_MenuBarBg, "Menu Bar"},
            }},
            {"Text", {
                {ImGuiCol_Text, "Text"},
                {ImGuiCol_TextDisabled, "Text Disabled"},
                {ImGuiCol_TextSelectedBg, "Text Selection"},
            }},
            {"Borders", {
                {ImGuiCol_Border, "Border"},
                {ImGuiCol_Separator, "Separator"},
                {ImGuiCol_SeparatorHovered, "Separator Hovered"},
                {ImGuiCol_SeparatorActive, "Separator Active"},
            }},
            {"Buttons", {
                {ImGuiCol_Button, "Button"},
                {ImGuiCol_ButtonHovered, "Button Hovered"},
                {ImGuiCol_ButtonActive, "Button Active"},
            }},
            {"Frames (Inputs)", {
                {ImGuiCol_FrameBg, "Frame"},
                {ImGuiCol_FrameBgHovered, "Frame Hovered"},
                {ImGuiCol_FrameBgActive, "Frame Active"},
            }},
            {"Headers (Rows)", {
                {ImGuiCol_Header, "Header"},
                {ImGuiCol_HeaderHovered, "Header Hovered"},
                {ImGuiCol_HeaderActive, "Header Active"},
            }},
            {"Tabs", {
                {ImGuiCol_Tab, "Tab"},
                {ImGuiCol_TabHovered, "Tab Hovered"},
                {ImGuiCol_TabSelected, "Tab Selected"},
            }},
            {"Title Bar", {
                {ImGuiCol_TitleBg, "Title"},
                {ImGuiCol_TitleBgActive, "Title Active"},
            }},
            {"Scrollbar", {
                {ImGuiCol_ScrollbarBg, "Background"},
                {ImGuiCol_ScrollbarGrab, "Grab"},
                {ImGuiCol_ScrollbarGrabHovered, "Grab Hovered"},
                {ImGuiCol_ScrollbarGrabActive, "Grab Active"},
            }},
            {"Tables", {
                {ImGuiCol_TableHeaderBg, "Header"},
                {ImGuiCol_TableBorderStrong, "Border Strong"},
                {ImGuiCol_TableBorderLight, "Border Light"},
                {ImGuiCol_TableRowBg, "Row"},
                {ImGuiCol_TableRowBgAlt, "Row Alt"},
            }},
            {"Accents", {
                {ImGuiCol_CheckMark, "Check Mark"},
                {ImGuiCol_SliderGrab, "Slider"},
                {ImGuiCol_SliderGrabActive, "Slider Active"},
                {ImGuiCol_NavHighlight, "Nav Highlight"},
                {ImGuiCol_ResizeGrip, "Resize Grip"},
                {ImGuiCol_ResizeGripHovered, "Resize Grip Hovered"},
                {ImGuiCol_ResizeGripActive, "Resize Grip Active"},
            }},
        };

        ImGui::BeginChild("##ColorEditor", ImVec2(0, -40), ImGuiChildFlags_Border);
        for (auto& group : groups) {
            if (ImGui::TreeNode(group.name)) {
                for (auto& [idx, label] : group.entries) {
                    if (ImGui::ColorEdit4(label, &c[idx].x,
                            ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreviewHalf)) {
                        m_theme.colors[idx] = c[idx];
                        changed = true;
                    }
                }
                ImGui::TreePop();
            }
        }
        ImGui::EndChild();
    }

    ImGui::Spacing();

    if (changed) {
        m_theme.save();
    }

    ImGui::End();
}

void App::renderDebugWindow() {
    ImGui::SetNextWindowSize(ImVec2(800, 450), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Debug Log (F12)", &m_showDebugWindow)) { ImGui::End(); return; }

    // Toolbar
    auto entries = DebugLog::instance().snapshot();
    auto startTime = entries.empty() ? std::chrono::steady_clock::now() : entries.front().time;

    // Pre-filter into indices
    std::vector<int> visible;
    visible.reserve(entries.size());
    for (int i = 0; i < (int)entries.size(); i++) {
        auto& e = entries[i];
        if ((int)e.level < m_debugLevelFilter) continue;
        if (m_debugTagFilter[0] && e.tag.find(m_debugTagFilter) == std::string::npos) continue;
        visible.push_back(i);
    }

    if (ImGui::Button("Copy All")) {
        std::string text = buildLogText(entries, visible, startTime);
        ImGui::SetClipboardText(text.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) DebugLog::instance().clear();
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &m_debugAutoScroll);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    const char* levels[] = { "All", "Info+", "Warn+", "Error" };
    ImGui::Combo("Level", &m_debugLevelFilter, levels, 4);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    ImGui::InputTextWithHint("##TagFilter", "Filter tag...", m_debugTagFilter, sizeof(m_debugTagFilter));
    ImGui::SameLine();
    ImGui::TextDisabled("(%d / %d entries)", (int)visible.size(), (int)entries.size());
    ImGui::Separator();

    // Log content area
    ImGui::BeginChild("##LogScroll", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);

    ImGuiListClipper clipper;
    clipper.Begin((int)visible.size());
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
            auto& e = entries[visible[row]];
            double secs = std::chrono::duration<double>(e.time - startTime).count();

            ImVec4 col;
            const char* lvl;
            switch (e.level) {
                case LogLevel::Debug: col = ImVec4(0.5f,0.5f,0.5f,1); lvl = "DBG"; break;
                case LogLevel::Info:  col = ImVec4(0.7f,0.9f,1.0f,1); lvl = "INF"; break;
                case LogLevel::Warn:  col = ImVec4(1.0f,0.8f,0.3f,1); lvl = "WRN"; break;
                case LogLevel::Error: col = ImVec4(1.0f,0.3f,0.3f,1); lvl = "ERR"; break;
                default:              col = ImVec4(1,1,1,1);           lvl = "???"; break;
            }

            // Build the line text for selection
            char timeBuf[32];
            { auto t = std::chrono::system_clock::to_time_t(e.wallTime);
              auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(e.wallTime.time_since_epoch()).count() % 1000;
              struct tm tm; localtime_s(&tm, &t);
              snprintf(timeBuf, sizeof(timeBuf), "[%02d:%02d:%02d.%03d] [%s] [%-8s] ",
                       tm.tm_hour, tm.tm_min, tm.tm_sec, (int)ms, lvl, e.tag.c_str()); }
            std::string lineText = std::string(timeBuf) + e.message;

            // Selectable row — right-click to copy individual line
            ImGui::PushID(row);
            if (ImGui::Selectable("##line", false, ImGuiSelectableFlags_AllowOverlap)) {
                ImGui::SetClipboardText(lineText.c_str());
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                ImGui::SetClipboardText(lineText.c_str());
            }
            ImGui::SameLine(0, 0);

            // Colored rendering on top
            { auto t2 = std::chrono::system_clock::to_time_t(e.wallTime);
              auto ms2 = std::chrono::duration_cast<std::chrono::milliseconds>(e.wallTime.time_since_epoch()).count() % 1000;
              struct tm tm2; localtime_s(&tm2, &t2);
              ImGui::TextDisabled("[%02d:%02d:%02d.%03d]", tm2.tm_hour, tm2.tm_min, tm2.tm_sec, (int)ms2); }
            ImGui::SameLine();
            ImGui::TextColored(col, "[%s]", lvl);
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f,0.7f,1,1), "[%-8s]", e.tag.c_str());
            ImGui::SameLine();
            if (e.level == LogLevel::Error)
                ImGui::TextColored(col, "%s", e.message.c_str());
            else
                ImGui::TextUnformatted(e.message.c_str());
            ImGui::PopID();
        }
    }
    clipper.End();

    if (m_debugAutoScroll && DebugLog::instance().m_scrollToBottom) {
        ImGui::SetScrollHereY(1.0f);
        DebugLog::instance().m_scrollToBottom = false;
    }

    ImGui::EndChild();
    ImGui::End();
}

void App::renderStatusBar() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.02f,0.02f,0.03f,1));
    float statusBarH = ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.y * 2.0f;
    ImGui::BeginChild("##StatusBar", ImVec2(0, statusBarH));
    float padY = (statusBarH - ImGui::GetFontSize()) * 0.5f;
    ImGui::SetCursorPos(ImVec2(8, padY));

    ImGui::TextDisabled("Windows: %d items", m_leftPanel.entryCount());
    ImGui::SameLine(0, 20);

    int ls = (int)m_leftPanel.selectedIndices.size();
    int rs = (int)m_rightPanel.selectedIndices.size();
    if (ls > 0) { ImGui::Text("| %d selected (PC)", ls); ImGui::SameLine(0, 20); }
    if (rs > 0) { ImGui::Text("| %d selected (Android)", rs); ImGui::SameLine(0, 20); }

    ImGui::TextDisabled("| Android: %d items", m_rightPanel.entryCount());
    ImGui::SameLine(0, 20);

    // Status message - show device client status if poll is active, otherwise app status
    {
        std::string devStatus = m_device.statusText();
        if (m_pollBusy && !devStatus.empty()) {
            // Live status from device client takes priority
            ImGui::TextColored(ImVec4(0.4f, 0.7f, 1, 1), "| %s", devStatus.c_str());
            ImGui::SameLine(0, 20);
        }
    }
    auto elapsed = std::chrono::steady_clock::now() - m_statusTime;
    if (!m_statusMessage.empty() && elapsed < std::chrono::seconds(15)) {
        ImGui::TextColored(ImVec4(0.3f,0.9f,0.5f,1), "| %s", m_statusMessage.c_str());
        ImGui::SameLine(0, 20);
    }

    // Show transfer indicator if overlay is hidden but transfer is active
    if (!m_overlayVisible) {
        std::lock_guard<std::mutex> lk(m_batchMutex);
        for (auto& b : m_batchQueue) {
            BatchState s = b->state.load();
            if (s == BatchState::Running || s == BatchState::Paused || s == BatchState::Verifying) {
                if (s == BatchState::Verifying) {
                    int ph = b->crcPhase.load();
                    if (ph == 1)
                        ImGui::TextColored(ImVec4(0.5f,0.4f,0.9f,1), "| Verifying: Device CRC...");
                    else {
                        float crcPct = b->crcProgress.load() * 100.0f;
                        ImGui::TextColored(ImVec4(0.5f,0.4f,0.9f,1), "| Verifying: Local %.0f%%", crcPct);
                    }
                } else {
                    float pct = b->totalProgress.load() * 100.0f;
                    ImGui::TextColored(ImVec4(0.4f,0.7f,1,1), "| Transfer: %.0f%%", pct);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Show")) m_overlayVisible = true;
                ImGui::SameLine(0, 20);
                break;
            }
        }
    }

    // Connection mode indicator - always show when device is selected
    if (m_selectedDevice >= 0) {
        std::string mode;
        if (!m_device.isServerRunning())
            mode = "[Connecting...]";
        else if (m_device.isDirectConnection())
            mode = "[Direct TCP: " + m_device.deviceIp() + "]";
        else
            mode = "[ADB Forward]";

        float tw = ImGui::CalcTextSize(mode.c_str()).x;
        ImGui::SameLine(ImGui::GetWindowWidth() - tw - 16);

        if (!m_device.isServerRunning()) {
            ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1), "%s", mode.c_str());
        } else if (m_device.isDirectConnection()) {
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.5f, 1), "%s", mode.c_str());
        } else {
            ImGui::TextUnformatted(mode.c_str());
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void App::renderContextMenu(FilePanel& panel) {
    if (ImGui::BeginPopup("##FileContextMenu")) {
        if (m_contextPanel == &panel && panel.validIndex(m_contextIndex)) {
            std::string name = panel.entryName(m_contextIndex);
            ImGui::TextDisabled("%s", name.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Open / Navigate")) {
                if (panel.entryIsDir(m_contextIndex)) {
                    std::string sep = panel.isAndroid ? "/" : "\\";
                    std::string np = panel.currentPath;
                    if (np.back() != sep[0]) np += sep; np += name;
                    navigateToDirectory(panel, np);
                }
            }
            if (!panel.isAndroid && !panel.entryIsDir(m_contextIndex)) {
                if (ImGui::MenuItem("Open with Default App")) {
                    std::string fp = panel.currentPath;
                    if (fp.back() != '\\') fp += "\\"; fp += name;
                    ShellExecuteA(nullptr, "open", fp.c_str(), nullptr, nullptr, SW_SHOW);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Copy", "Ctrl+C")) {
                m_clipboardPaths.clear();
                std::string sep = panel.isAndroid ? "/" : "\\";
                for (int idx : panel.selectedIndices) {
                    if (!panel.validIndex(idx)) continue;
                    std::string p = panel.currentPath;
                    if (p.back() != sep[0]) p += sep;
                    p += panel.entryName(idx);
                    m_clipboardPaths.push_back(p);
                }
                m_clipboardIsAndroid = panel.isAndroid;
                m_clipboardDeviceSlot = panel.deviceSlot;
                m_clipboardCut = false;
            }
            if (ImGui::MenuItem("Cut", "Ctrl+X")) {
                m_clipboardPaths.clear();
                std::string sep = panel.isAndroid ? "/" : "\\";
                for (int idx : panel.selectedIndices) {
                    if (!panel.validIndex(idx)) continue;
                    std::string p = panel.currentPath;
                    if (p.back() != sep[0]) p += sep;
                    p += panel.entryName(idx);
                    m_clipboardPaths.push_back(p);
                }
                m_clipboardIsAndroid = panel.isAndroid;
                m_clipboardDeviceSlot = panel.deviceSlot;
                m_clipboardCut = true;
            }
            if (!m_clipboardPaths.empty()) {
                const char* pasteLabel = m_clipboardCut ? "Move Here" : "Paste";
                if (ImGui::MenuItem(pasteLabel, "Ctrl+V")) {
                    performClipboardPaste(panel);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Copy to Other Panel")) {
                if (&panel == &m_leftPanel) startTransfer(false); else startTransfer(true);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Rename...")) { strcpy_s(m_renameBuf, name.c_str()); m_showRenamePopup = true; }
            if (ImGui::MenuItem("Delete", "Del")) {
                m_deletePermanent = ImGui::GetIO().KeyShift;
                m_showDeleteConfirm = true;
            }
            if (ImGui::MenuItem("Delete Permanently", "Shift+Del")) {
                m_deletePermanent = true;
                m_showDeleteConfirm = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("New Folder...")) {
                memset(m_newFolderName, 0, sizeof(m_newFolderName));
                m_contextPanel = &panel; m_showNewFolderPopup = true;
            }
        }
        ImGui::EndPopup();
    }
}

void App::renderNewFolderPopup() {
    if (m_showNewFolderPopup) { ImGui::OpenPopup("New Folder"); m_showNewFolderPopup = false; m_newFolderNeedsFocus = true; }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f,0.5f));
    if (ImGui::BeginPopupModal("New Folder", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Enter folder name:"); ImGui::SetNextItemWidth(300);
        if (m_newFolderNeedsFocus) { ImGui::SetKeyboardFocusHere(); m_newFolderNeedsFocus = false; }
        bool enter = ImGui::InputText("##NFN", m_newFolderName, sizeof(m_newFolderName), ImGuiInputTextFlags_EnterReturnsTrue);
        if ((ImGui::Button("Create", ImVec2(120,0)) || enter) && strlen(m_newFolderName) > 0 && m_contextPanel) {
            if (m_contextPanel->isAndroid) {
                std::string mkdirPath = m_contextPanel->currentPath + "/" + m_newFolderName;
                if (m_selectedDevice >= 0) { int slot = m_contextPanel->deviceSlot; postAsync("Creating folder...", [this, mkdirPath, slot]() { deviceForSlot(slot).createDirectory(mkdirPath); }); }
            } else {
                std::string p = m_contextPanel->currentPath; if (p.back()!='\\') p+="\\"; p+=m_newFolderName;
                std::filesystem::create_directories(p);
            }
            m_contextPanel->needsRefresh = true; ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120,0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void App::renderRenamePopup() {
    if (m_showRenamePopup) { ImGui::OpenPopup("Rename"); m_showRenamePopup = false; m_renameNeedsSelect = true; }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f,0.5f));
    if (ImGui::BeginPopupModal("Rename", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("New name:"); ImGui::SetNextItemWidth(300);
        if (m_renameNeedsSelect) ImGui::SetKeyboardFocusHere();
        auto renameCb = [](ImGuiInputTextCallbackData* data) -> int {
            bool* needsSelect = (bool*)data->UserData;
            if (*needsSelect) {
                *needsSelect = false;
                const char* dot = strrchr(data->Buf, '.');
                int selectEnd = dot ? (int)(dot - data->Buf) : data->BufTextLen;
                data->SelectionStart = 0;
                data->SelectionEnd = selectEnd;
                data->CursorPos = selectEnd;
            }
            return 0;
        };
        bool enter = ImGui::InputText("##RN", m_renameBuf, sizeof(m_renameBuf),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways,
            renameCb, &m_renameNeedsSelect);
        if ((ImGui::Button("Rename", ImVec2(120,0)) || enter) && m_contextPanel && m_contextPanel->validIndex(m_contextIndex)) {
            std::string oldN = m_contextPanel->entryName(m_contextIndex), newN(m_renameBuf);
            if (!newN.empty() && newN != oldN) {
                if (m_contextPanel->isAndroid && m_selectedDevice >= 0) {
                    std::string fromP = m_contextPanel->currentPath+"/"+oldN, toP = m_contextPanel->currentPath+"/"+newN;
                    int slot = m_contextPanel->deviceSlot; postAsync("Renaming...", [this, fromP, toP, slot]() { deviceForSlot(slot).renameFile(fromP, toP); });
                } else {
                    std::string bp = m_contextPanel->currentPath; if (bp.back()!='\\') bp+="\\";
                    try { std::filesystem::rename(toFsPath(bp+oldN), toFsPath(bp+newN)); } catch (const std::exception& e) {
                        m_statusMessage = std::string("Rename failed: ") + e.what(); m_statusTime = std::chrono::steady_clock::now();
                    }
                }
                m_contextPanel->needsRefresh = true;
            }
            m_contextIndex = -1; m_contextPanel = nullptr; ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120,0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void App::renderDeleteConfirmPopup() {
    if (m_showDeleteConfirm) { ImGui::OpenPopup("Confirm Delete"); m_showDeleteConfirm = false; }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Confirm Delete", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (m_contextPanel && !m_contextPanel->selectedIndices.empty()) {
            int selCount = (int)m_contextPanel->selectedIndices.size();
            bool isAndroid = m_contextPanel->isAndroid;

            // Warning header
            if (m_deletePermanent || isAndroid) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.3f, 0.3f, 1));
                ImGui::Text("PERMANENTLY DELETE - This cannot be undone!");
                ImGui::PopStyleColor();
            } else {
                ImGui::TextColored(ImVec4(0.5f, 0.7f, 1, 1), "Move to Recycle Bin:");
            }
            ImGui::Spacing();

            // Show file names (up to 10, then summarize)
            int shown = 0;
            for (int idx : m_contextPanel->selectedIndices) {
                if (!m_contextPanel->validIndex(idx)) continue;
                if (shown < 10) {
                    std::string n = m_contextPanel->entryName(idx);
                    ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "  %s", n.c_str());
                }
                shown++;
            }
            if (selCount > 10) {
                ImGui::TextDisabled("  ... and %d more", selCount - 10);
            }

            ImGui::Spacing();
            if (selCount > 1) {
                ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "%d items will be %s",
                    selCount, (m_deletePermanent || isAndroid) ? "permanently deleted" : "moved to Recycle Bin");
                ImGui::Spacing();
            }

            // Hint about Shift
            if (!isAndroid && !m_deletePermanent) {
                ImGui::TextDisabled("Tip: Hold Shift for permanent delete");
                ImGui::Spacing();
            }

            // Delete button
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.15f, 0.15f, 1));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.2f, 0.2f, 1));
            const char* btnLabel = (m_deletePermanent || isAndroid) ? "Delete Permanently" : "Move to Recycle Bin";
            if (ImGui::Button(btnLabel, ImVec2(180, 0))) {
                if (isAndroid) {
                    // Android: always permanent via server — run async
                    std::vector<std::string> delPaths;
                    for (int idx : m_contextPanel->selectedIndices) {
                        if (!m_contextPanel->validIndex(idx)) continue;
                        delPaths.push_back(m_contextPanel->currentPath + "/" + m_contextPanel->entryName(idx));
                    }
                    if (m_selectedDevice >= 0 && !delPaths.empty()) {
                        int slot = m_contextPanel->deviceSlot;
                        postAsync("Deleting files...", [this, delPaths, slot]() {
                            for (auto& p : delPaths) deviceForSlot(slot).deleteFile(p);
                        });
                    }
                } else {
                    // Windows: use IFileOperation (recycle bin or permanent)
                    std::vector<std::string> paths;
                    for (int idx : m_contextPanel->selectedIndices) {
                        if (!m_contextPanel->validIndex(idx)) continue;
                        std::string p = m_contextPanel->currentPath;
                        if (p.back() != '\\') p += "\\";
                        p += m_contextPanel->entryName(idx);
                        paths.push_back(p);
                    }
                    std::string error;
                    if (!deleteWindowsFiles(paths, m_deletePermanent, error)) {
                        m_statusMessage = "Delete failed: " + error;
                        m_statusTime = std::chrono::steady_clock::now();
                    }
                }
                m_contextPanel->needsRefresh = true;
                m_contextIndex = -1; m_contextPanel = nullptr;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                m_contextIndex = -1; m_contextPanel = nullptr;
                ImGui::CloseCurrentPopup();
            }
        } else {
            ImGui::Text("No items selected.");
            if (ImGui::Button("Close", ImVec2(120, 0))) {
                m_contextIndex = -1; m_contextPanel = nullptr;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
}

void App::performClipboardPaste(FilePanel& dstPanel) {
    if (m_clipboardPaths.empty()) return;

    bool dstIsAndroid = dstPanel.isAndroid;
    bool srcIsAndroid = m_clipboardIsAndroid;
    bool isCut = m_clipboardCut;
    std::vector<std::string> paths = m_clipboardPaths;
    std::string dstDir = dstPanel.currentPath;
    int srcSlot = m_clipboardDeviceSlot;
    int dstSlot = dstPanel.deviceSlot;

    if (!srcIsAndroid && !dstIsAndroid) {
        // Windows to Windows — local filesystem copy/move
        postAsync(isCut ? "Moving files..." : "Copying files...", [this, paths, dstDir, isCut]() {
            for (auto& src : paths) {
                try {
                    auto srcPath = toFsPath(src);
                    std::string filename = pathToUtf8(srcPath.filename());
                    std::string dst = dstDir;
                    if (dst.back() != '\\') dst += "\\";
                    dst += filename;
                    auto dstPath = toFsPath(dst);
                    if (isCut) {
                        std::filesystem::rename(srcPath, dstPath);
                    } else {
                        if (std::filesystem::is_directory(srcPath))
                            std::filesystem::copy(srcPath, dstPath, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
                        else
                            std::filesystem::copy_file(srcPath, dstPath, std::filesystem::copy_options::overwrite_existing);
                    }
                } catch (const std::exception& e) {
                    m_statusMessage = std::string("Paste failed: ") + e.what();
                    m_statusTime = std::chrono::steady_clock::now();
                }
            }
        });
        dstPanel.needsRefresh = true;
        if (isCut) m_clipboardPaths.clear();
    } else if (srcIsAndroid && dstIsAndroid && srcSlot == dstSlot) {
        // Same Android device — move via rename, copy via adb shell cp
        postAsync(isCut ? "Moving files..." : "Copying files...", [this, paths, dstDir, isCut, srcSlot]() {
            for (auto& src : paths) {
                std::string filename = src.substr(src.rfind('/') + 1);
                std::string dst = dstDir;
                if (dst.back() != '/') dst += "/";
                dst += filename;
                if (isCut) {
                    deviceForSlot(srcSlot).renameFile(src, dst);
                } else {
                    // No server-side copy command; use adb shell cp -r
                    std::string cmd = "shell cp -r \"" + src + "\" \"" + dst + "\"";
                    deviceForSlot(srcSlot).runAdbCommand(cmd);
                }
            }
        });
        dstPanel.needsRefresh = true;
        if (isCut) m_clipboardPaths.clear();
    } else {
        // Cross-platform or cross-device — use the transfer system
        // Build a temporary selection in a virtual source panel and trigger transfer
        // For now, show a message directing users to use Copy to Other Panel
        m_statusMessage = "Cross-platform paste: use Copy to Other Panel or drag-and-drop";
        m_statusTime = std::chrono::steady_clock::now();
    }
}

void App::refreshWindowsPanel(FilePanel& panel) {
    panel.windowsEntries.clear(); panel.selectedIndices.clear();
    try {
        for (const auto& entry : std::filesystem::directory_iterator(toFsPath(panel.currentPath), std::filesystem::directory_options::skip_permission_denied)) {
            WindowsFileEntry fe;
            fe.name = pathToUtf8(entry.path().filename());
            fe.isDirectory = entry.is_directory();
            DWORD attrs = GetFileAttributesW(entry.path().wstring().c_str());
            fe.isHidden = (attrs != INVALID_FILE_ATTRIBUTES) && (attrs & FILE_ATTRIBUTE_HIDDEN);
            if (!fe.isDirectory) { try { fe.size = entry.file_size(); } catch (...) { fe.size = 0; } }
            try {
                auto ft = entry.last_write_time();
                auto sc = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ft - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
                auto t = std::chrono::system_clock::to_time_t(sc);
                struct tm tm; localtime_s(&tm, &t);
                char buf[32]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
                fe.dateModified = buf;
            } catch (...) { fe.dateModified = ""; }
            panel.windowsEntries.push_back(std::move(fe));
        }
    } catch (const std::exception& e) { m_statusMessage = std::string("Error: ")+e.what(); m_statusTime = std::chrono::steady_clock::now(); }
    int col = panel.sortColumn;
    bool desc = panel.sortDescending;
    std::sort(panel.windowsEntries.begin(), panel.windowsEntries.end(),
        [col, desc](const WindowsFileEntry& a, const WindowsFileEntry& b) {
            if (a.isDirectory != b.isDirectory) return a.isDirectory > b.isDirectory;
            int cmp;
            if (col == 1) cmp = (a.size < b.size) ? -1 : (a.size > b.size) ? 1 : 0;
            else if (col == 2) cmp = a.dateModified.compare(b.dateModified);
            else cmp = _stricmp(a.name.c_str(), b.name.c_str());
            return desc ? cmp > 0 : cmp < 0;
        });
    strcpy_s(panel.pathInput, panel.currentPath.c_str());
}

void App::refreshAndroidPanel(FilePanel& panel) {
    panel.androidEntries.clear(); panel.selectedIndices.clear();
    DeviceClient& dev = deviceFor(panel);
    if (!dev.isServerRunning()) return;

    if (panel.insideMcraw) {
        panel.androidEntries = dev.listMcraw(panel.mcrawFilePath);
    } else {
        panel.androidEntries = dev.listDirectory(panel.currentPath);
    }
    int col = panel.sortColumn;
    bool desc = panel.sortDescending;
    std::sort(panel.androidEntries.begin(), panel.androidEntries.end(),
        [col, desc](const DeviceFileEntry& a, const DeviceFileEntry& b) {
            if (a.isDirectory() != b.isDirectory()) return a.isDirectory();
            int cmp;
            if (col == 1) cmp = (a.size < b.size) ? -1 : (a.size > b.size) ? 1 : 0;
            else if (col == 2) cmp = (a.mtime < b.mtime) ? -1 : (a.mtime > b.mtime) ? 1 : 0;
            else cmp = _stricmp(a.name.c_str(), b.name.c_str());
            return desc ? cmp > 0 : cmp < 0;
        });
    strcpy_s(panel.pathInput, panel.currentPath.c_str());
}

void App::navigateToDirectory(FilePanel& panel, const std::string& path) {
    // Push to navigation history (truncate forward history)
    if (panel.navHistoryPos < 0 || panel.navHistory.empty() || panel.navHistory[panel.navHistoryPos] != path) {
        if (panel.navHistoryPos >= 0 && panel.navHistoryPos < (int)panel.navHistory.size() - 1)
            panel.navHistory.erase(panel.navHistory.begin() + panel.navHistoryPos + 1, panel.navHistory.end());
        panel.navHistory.push_back(path);
        panel.navHistoryPos = (int)panel.navHistory.size() - 1;
        if (panel.navHistory.size() > 50) { panel.navHistory.erase(panel.navHistory.begin()); panel.navHistoryPos--; }
    }
    panel.currentPath = path; panel.selectedIndices.clear(); panel.focusedIndex = -1;
    panel.searchFilter[0] = '\0'; panel.needsRefresh = true;

    // Detect MCRAW virtual directory
    if (endsWithCI(path, ".mcraw")) {
        panel.insideMcraw = true;
        panel.mcrawFilePath = path;
    } else if (panel.insideMcraw) {
        // Keep insideMcraw if we're still within a ProjFS mount (don't reset on sub-navigation)
        // Only clear if we navigated to the parent of the .mcraw file (exited the mount)
        std::filesystem::path mcrawParent = toFsPath(panel.mcrawFilePath).parent_path();
        std::filesystem::path curPath = toFsPath(path);
        // If current path IS the mcraw parent or above it, we've exited
        if (path == pathToUtf8(mcrawParent) || !pathToUtf8(curPath).starts_with(pathToUtf8(mcrawParent))) {
            // Check if we're still inside the mount point (ProjFS path won't contain the mcraw parent)
            // Simple heuristic: if path doesn't start with temp dir, we've left the mount
            char tmpDir[MAX_PATH]; GetTempPathA(MAX_PATH, tmpDir);
            std::string tempBase(tmpDir);
            bool inMountPath = (path.find(tempBase) == 0 || path.find("McrawMount") != std::string::npos);
            if (!inMountPath) {
                // Unmount the ProjFS mount when leaving the MCRAW
                McrawMountManager::instance().unmountAll();
                panel.insideMcraw = false;
                panel.mcrawFilePath.clear();
            }
        }
    }

    // Trigger fade-in animation
    if (&panel == &m_leftPanel) m_lastNavTimeLeft = std::chrono::steady_clock::now();
    else m_lastNavTimeRight = std::chrono::steady_clock::now();
}

void App::switchPanelMode(FilePanel& panel, bool toAndroid) {
    if (panel.isAndroid == toAndroid) return;
    panel.isAndroid = toAndroid;
    panel.selectedIndices.clear();
    panel.focusedIndex = -1;
    panel.searchFilter[0] = '\0';
    if (panel.insideMcraw) McrawMountManager::instance().unmountAll();
    panel.insideMcraw = false;
    panel.mcrawFilePath.clear();
    panel.navHistory.clear();
    panel.navHistoryPos = -1;
    if (toAndroid) {
        panel.windowsEntries.clear();
        // If the other panel is already Android on slot 0, use slot 1
        FilePanel& other = (&panel == &m_leftPanel) ? m_rightPanel : m_leftPanel;
        if (other.isAndroid && other.deviceSlot == 0 && m_slotConnected[1])
            panel.deviceSlot = 1;
        else
            panel.deviceSlot = 0;
        std::string root = m_slotStorageRoot[panel.deviceSlot];
        panel.currentPath = root.empty() ? "/" : root;
        strcpy_s(panel.pathInput, panel.currentPath.c_str());
        panel.needsRefresh = true;
    } else {
        panel.androidEntries.clear();
        panel.currentPath = "C:\\";
        strcpy_s(panel.pathInput, panel.currentPath.c_str());
        panel.needsRefresh = true;
    }
}

void App::forEachAndroidPanel(std::function<void(FilePanel&)> fn) {
    if (m_leftPanel.isAndroid) fn(m_leftPanel);
    if (m_rightPanel.isAndroid) fn(m_rightPanel);
}

void App::navigateUp(FilePanel& panel) {
    if (panel.insideMcraw) {
        // Exit MCRAW virtual directory: go to the parent folder containing the .mcraw file
        std::string parent;
        if (panel.isAndroid) {
            auto pos = panel.mcrawFilePath.rfind('/');
            parent = (pos != std::string::npos && pos > 0) ? panel.mcrawFilePath.substr(0, pos) : "/";
        } else {
            auto p = toFsPath(panel.mcrawFilePath);
            parent = pathToUtf8(p.parent_path());
        }
        navigateToDirectory(panel, parent);
        return;
    }

    std::string parent;
    if (panel.isAndroid) {
        auto pos = panel.currentPath.rfind('/');
        parent = (pos != std::string::npos && pos > 0) ? panel.currentPath.substr(0, pos) : "/";
    } else {
        auto p = toFsPath(panel.currentPath);
        parent = (p.has_parent_path() && p.parent_path() != p) ? pathToUtf8(p.parent_path()) : panel.currentPath;
    }
    if (parent != panel.currentPath)
        navigateToDirectory(panel, parent);
}

void App::devicePollLoop() {
    // Find ADB and clean start on background thread
    m_pollBusy = true;
    m_statusMessage = "Finding ADB...";
    m_statusTime = std::chrono::steady_clock::now();

    m_device.findAdb();
    // Share ADB path with secondary device client
    if (!m_device.getAdbPath().empty())
        m_deviceSlots[1].setAdbPath(m_device.getAdbPath());

    if (!m_device.getAdbPath().empty()) {
        if (m_prefs.restartAdbOnLaunch) {
            m_statusMessage = "Restarting ADB daemon...";
            m_statusTime = std::chrono::steady_clock::now();
            LOG_INFO("ADB", "Restarting ADB daemon (preference enabled)");
            m_device.runAdbCommand("kill-server");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            m_device.runAdbCommand("start-server");
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
        m_statusMessage = "ADB ready - waiting for device...";
        m_statusTime = std::chrono::steady_clock::now();
    } else {
        m_statusMessage = "ADB not found - install Android SDK Platform Tools";
        m_statusTime = std::chrono::steady_clock::now();
    }
    m_pollBusy = false;

    // Helper to check if any batch is active
    auto isBatchActive = [&]() -> bool {
        std::lock_guard<std::mutex> lk(m_batchMutex);
        for (auto& b : m_batchQueue) {
            auto s = b->state.load();
            if (s == BatchState::Running || s == BatchState::Paused || s == BatchState::Queued || s == BatchState::Verifying) return true;
        }
        return false;
    };

    // Helper to process a device list update (from track-devices or getDevices)
    auto processDeviceUpdate = [&](const std::vector<DeviceInfo>& devices) {
        m_pollBusy = true;
        std::string curSerial;
        {
            std::lock_guard<std::mutex> lk(m_deviceMutex);
            m_devices = devices;

            // Build list of online devices only
            auto findOnline = [&](bool preferUsb) -> int {
                // First pass: preferred type
                for (int i = 0; i < (int)m_devices.size(); i++) {
                    if (m_devices[i].state != "device") continue;
                    bool isWifi = isWifiSerial(m_devices[i].serial);
                    if (preferUsb ? !isWifi : isWifi) return i;
                }
                // Second pass: any online device
                for (int i = 0; i < (int)m_devices.size(); i++)
                    if (m_devices[i].state == "device") return i;
                return -1;
            };

            // Check if currently selected device went offline
            bool selectedOffline = (m_selectedDevice >= 0 && m_selectedDevice < (int)m_devices.size() &&
                                    m_devices[m_selectedDevice].state != "device");
            bool selectedGone = (m_selectedDevice >= 0 && m_selectedDevice >= (int)m_devices.size());

            if (m_selectedDevice < 0 || selectedOffline || selectedGone) {
                // Release keep-awake if device went away (process death auto-releases the wakelock on the device)
                if ((selectedOffline || selectedGone) && m_keepAwake) {
                    if (m_wakeLockProcess) {
                        TerminateProcess(m_wakeLockProcess, 0);
                        CloseHandle(m_wakeLockProcess);
                        m_wakeLockProcess = nullptr;
                    }
                    m_keepAwake = false;
                }
                // Try to find the same physical device via the other serial
                int found = -1;
                if (selectedOffline || selectedGone) {
                    std::string lostSerial = (selectedOffline && m_selectedDevice < (int)m_devices.size())
                        ? m_devices[m_selectedDevice].serial : m_lastDeviceSerial;
                    // Check if the partner serial (USB↔WiFi) is online
                    std::string partnerSerial;
                    if (isWifiSerial(lostSerial) && !m_wifiToUsbSerial.empty())
                        partnerSerial = m_wifiToUsbSerial;
                    else if (!isWifiSerial(lostSerial) && !m_usbToWifiSerial.empty())
                        partnerSerial = m_usbToWifiSerial;

                    if (!partnerSerial.empty()) {
                        for (int i = 0; i < (int)m_devices.size(); i++) {
                            if (m_devices[i].serial == partnerSerial && m_devices[i].state == "device") {
                                found = i;
                                LOG_INFO("Poll", "Auto-switching to partner serial: " + partnerSerial);
                                break;
                            }
                        }
                    }
                }
                m_selectedDevice = (found >= 0) ? found : findOnline(true);
            }

            curSerial = (m_selectedDevice >= 0 && m_selectedDevice < (int)m_devices.size()
                                     && m_devices[m_selectedDevice].state == "device")
                ? m_devices[m_selectedDevice].serial : "";

            // If server is still running and serial just flipped USB↔WiFi, keep it
            if (!m_lastDeviceSerial.empty() && !curSerial.empty() &&
                curSerial != m_lastDeviceSerial && m_device.isServerRunning()) {
                // Check if old serial is still online
                for (int i = 0; i < (int)m_devices.size(); i++) {
                    if (m_devices[i].serial == m_lastDeviceSerial && m_devices[i].state == "device") {
                        m_selectedDevice = i;
                        curSerial = m_lastDeviceSerial;
                        break;
                    }
                }
            }
        }

        // Resolve marketing display names for new devices (outside lock, may call adb)
        for (auto& d : devices) {
            if (d.state != "device") continue;
            if (m_deviceDisplayNames.count(d.serial)) continue;
            // First check saved prefs for a cached name
            for (auto& w : m_prefs.savedWifiDevices) {
                if (!w.model.empty() && (w.serial == d.serial ||
                    (w.wifiIp + ":" + std::to_string(w.port)) == d.serial)) {
                    m_deviceDisplayNames[d.serial] = w.model;
                    break;
                }
            }
            if (m_deviceDisplayNames.count(d.serial)) continue;
            // Query the device for its marketing name
            std::string name = queryDeviceDisplayName(d.serial);
            if (!name.empty())
                m_deviceDisplayNames[d.serial] = name;
        }

        // Detect the other connection type appearing → upgrade to true dual channel
        // Case 1: On WiFi, USB cable plugged in → add USB as secondary channel (keep WiFi primary)
        // Triggers when: no dual channel yet, OR dual channel is WiFi-only (upgrade WiFi+WiFi to WiFi+USB)
        if (isWifiSerial(curSerial) && m_device.isServerRunning() &&
            (!m_dualChannelAvailable || m_secondaryChannelType != "USB")) {
            // Find a USB device in the list that belongs to the same physical device
            std::string usbSerial;
            {
                std::lock_guard<std::mutex> lk2(m_deviceMutex);
                // First: try known mapping
                if (!m_wifiToUsbSerial.empty() && curSerial == m_usbToWifiSerial) {
                    for (auto& d : m_devices) {
                        if (d.serial == m_wifiToUsbSerial && d.state == "device") {
                            usbSerial = m_wifiToUsbSerial;
                            break;
                        }
                    }
                }
                // Second: find USB device with same model name, but ONLY if there's
                // exactly one other device (otherwise we can't tell which USB belongs
                // to this WiFi device and would grab a different device's USB)
                if (usbSerial.empty()) {
                    int onlineUsbCount = 0;
                    for (auto& d : m_devices)
                        if (d.state == "device" && !isWifiSerial(d.serial)) onlineUsbCount++;

                    if (onlineUsbCount == 1) {
                        std::string wifiModel;
                        for (auto& d : m_devices)
                            if (d.serial == curSerial) { wifiModel = d.model; break; }
                        if (!wifiModel.empty()) {
                            for (auto& d : m_devices) {
                                if (d.state == "device" && !isWifiSerial(d.serial) && d.model == wifiModel) {
                                    usbSerial = d.serial;
                                    break;
                                }
                            }
                        }
                    } else if (onlineUsbCount > 1) {
                        LOG_INFO("Poll", "Multiple USB devices online, skipping model-based dual channel pairing to avoid grabbing wrong device");
                    }
                }
            }
            if (!usbSerial.empty()) {
                LOG_INFO("Poll", "USB cable connected while on WiFi - adding USB secondary channel via " + usbSerial);
                // Update mapping
                m_wifiToUsbSerial = usbSerial;
                m_usbToWifiSerial = curSerial;
                // Disconnect existing WiFi secondary if upgrading
                if (m_dualChannelAvailable && m_secondaryChannelType != "USB") {
                    m_secondaryChannel.disconnectTcp();
                    m_dualChannelAvailable = false;
                    LOG_INFO("Poll", "Disconnected WiFi secondary to upgrade to USB");
                }
                // Set up ADB forward on the USB serial and connect secondary channel through it
                std::string fwdResult = m_device.runAdbCommand("-s " + usbSerial + " forward tcp:5741 tcp:5740");
                if (fwdResult.find("error") == std::string::npos) {
                    m_secondaryChannel.setAdbPath(m_device.getAdbPath());
                    if (m_secondaryChannel.connectTcp("127.0.0.1", 5741) && m_secondaryChannel.verifyConnection()) {
                        m_dualChannelAvailable = true;
                        m_secondaryChannelType = "USB";
                        m_statusMessage = "Dual channel active: WiFi + USB";
                        LOG_INFO("Poll", "Secondary USB channel established via ADB forward");
                    } else {
                        // Clean up the forward if connection failed
                        m_device.runAdbCommand("-s " + usbSerial + " forward --remove tcp:5741");
                        LOG_WARN("Poll", "USB secondary channel connection failed");
                        // Re-establish WiFi secondary as fallback
                        std::string wifiIp;
                        auto colon = curSerial.rfind(':');
                        if (colon != std::string::npos) wifiIp = curSerial.substr(0, colon);
                        if (!wifiIp.empty() && m_secondaryChannel.connectTcp(wifiIp, AFM_PORT) &&
                            m_secondaryChannel.verifyConnection()) {
                            m_dualChannelAvailable = true;
                            m_secondaryChannelType = "WiFi";
                            LOG_INFO("Poll", "Re-established WiFi secondary as fallback");
                        }
                    }
                } else {
                    LOG_WARN("Poll", "ADB forward for USB secondary failed: " + fwdResult);
                }
            }
        }
        // Case 1b: Secondary is USB but USB device disappeared → downgrade to WiFi secondary
        if (m_dualChannelAvailable && m_secondaryChannelType == "USB" &&
            isWifiSerial(curSerial) && !isBatchActive()) {
            bool usbStillOnline = false;
            {
                std::lock_guard<std::mutex> lk2(m_deviceMutex);
                if (!m_wifiToUsbSerial.empty()) {
                    for (auto& d : m_devices) {
                        if (d.serial == m_wifiToUsbSerial && d.state == "device") {
                            usbStillOnline = true;
                            break;
                        }
                    }
                }
            }
            if (!usbStillOnline) {
                LOG_INFO("Poll", "USB device gone - downgrading secondary from USB to WiFi");
                m_secondaryChannel.disconnectTcp();
                // Clean up ADB forward
                if (!m_wifiToUsbSerial.empty())
                    m_device.runAdbCommand("-s " + m_wifiToUsbSerial + " forward --remove tcp:5741");
                // Re-establish via WiFi
                std::string wifiIp;
                auto colon = curSerial.rfind(':');
                if (colon != std::string::npos) wifiIp = curSerial.substr(0, colon);
                if (!wifiIp.empty() && m_secondaryChannel.connectTcp(wifiIp, AFM_PORT) &&
                    m_secondaryChannel.verifyConnection()) {
                    m_secondaryChannelType = "WiFi";
                    m_statusMessage = "Dual channel: WiFi + WiFi";
                    m_statusTime = std::chrono::steady_clock::now();
                    LOG_INFO("Poll", "Re-established WiFi secondary after USB disconnect");
                } else {
                    m_dualChannelAvailable = false;
                    m_secondaryChannelType.clear();
                    LOG_WARN("Poll", "Failed to re-establish WiFi secondary after USB disconnect");
                    m_statusMessage = "USB disconnected, dual channel disabled";
                    m_statusTime = std::chrono::steady_clock::now();
                }
            }
        }

        // Case 2: On USB, WiFi becomes available → add WiFi as secondary channel
        // Only if the WiFi serial belongs to the SAME device as the current USB serial
        if (m_prefs.wifiAutoConnect && !isWifiSerial(curSerial) && !curSerial.empty() && m_device.isServerRunning() &&
            !m_dualChannelAvailable && !m_usbToWifiSerial.empty() && curSerial == m_wifiToUsbSerial) {
            std::lock_guard<std::mutex> lk2(m_deviceMutex);
            for (int i = 0; i < (int)m_devices.size(); i++) {
                if (m_devices[i].serial == m_usbToWifiSerial && m_devices[i].state == "device") {
                    // WiFi serial appeared — try to establish secondary channel directly
                    LOG_INFO("Poll", "WiFi appeared while on USB - adding WiFi secondary channel");
                    std::string wifiIp;
                    auto colon = m_usbToWifiSerial.rfind(':');
                    if (colon != std::string::npos) wifiIp = m_usbToWifiSerial.substr(0, colon);
                    if (!wifiIp.empty()) {
                        m_secondaryChannel.setAdbPath(m_device.getAdbPath());
                        if (m_secondaryChannel.connectTcp(wifiIp, AFM_PORT) && m_secondaryChannel.verifyConnection()) {
                            m_dualChannelAvailable = true;
                            m_secondaryChannelType = "WiFi";
                            m_statusMessage = "Dual channel active: USB + WiFi";
                            LOG_INFO("Poll", "Secondary WiFi channel established: " + wifiIp);
                        }
                    }
                    break;
                }
            }
        }

        if (curSerial != m_lastDeviceSerial) {
            LOG_INFO("Poll", "Serial changed: '" + m_lastDeviceSerial + "' -> '" + curSerial + "'");

            // Don't treat USB↔WiFi flip as disconnect if server is still running
            // BUT allow the upgrade from WiFi-only to USB+WiFi
            if (!curSerial.empty() && !m_lastDeviceSerial.empty() &&
                isWifiSerial(curSerial) != isWifiSerial(m_lastDeviceSerial) &&
                m_device.isServerRunning()) {
                LOG_INFO("Poll", "USB/WiFi serial flip - server still connected, skipping reconnect");
                m_lastDeviceSerial = curSerial;
                m_pollBusy = false;
                return;
            }

            if (curSerial.empty() && !m_lastDeviceSerial.empty()) {
                LOG_WARN("Poll", "Device disconnected");
                m_wifiProbeAttempts = 0; // reset probe backoff for fast reconnect
                m_device.disconnectTcp(); // immediately kill TCP so reconnect doesn't think server is still up
                m_statusMessage = "Device disconnected";
                m_statusTime = std::chrono::steady_clock::now();
                m_androidStorageRoot.clear();
                m_androidVolumes.clear();
                m_slotStorageRoot[0].clear();
                m_slotVolumes[0].clear();
                m_slotConnected[0] = false;
                forEachAndroidPanel([](FilePanel& p) {
                    p.androidEntries.clear();
                    p.selectedIndices.clear();
                    p.currentPath = "/";
                    strcpy_s(p.pathInput, "/");
                });
            } else if (!curSerial.empty()) {
                if (m_device.isServerRunning() && m_device.connectedSerial() == curSerial) {
                    LOG_INFO("Poll", "Device " + curSerial + " already connected, skipping init");
                    m_lastDeviceSerial = curSerial;
                    m_pollBusy = false;
                    return;
                }
                std::string model;
                {
                    std::lock_guard<std::mutex> lk(m_deviceMutex);
                    model = (m_selectedDevice >= 0 && m_selectedDevice < (int)m_devices.size())
                        ? m_devices[m_selectedDevice].model : curSerial;
                }
                m_statusMessage = "Device detected: " + model + " - initializing...";
                m_statusTime = std::chrono::steady_clock::now();
                std::this_thread::sleep_for(std::chrono::seconds(1)); // brief settle
                if (m_shutdownPoll) return;
                // Don't restart server during active transfers - it kills existing connections
                if (isBatchActive()) {
                    LOG_INFO("Poll", "Skipping onDeviceChanged - transfer active");
                    m_lastDeviceSerial = curSerial;
                } else {
                    LOG_INFO("Poll", "Calling onDeviceChanged()");
                    onDeviceChanged();
                }
            }
            m_lastDeviceSerial = curSerial;
        }

        // Retry server connection if needed
        if (!curSerial.empty() && !m_device.isServerRunning() && !isBatchActive()) {
            if (!isBatchActive()) {
                LOG_INFO("Poll", "Device present but server not running - retrying");
                m_statusMessage = "Retrying server connection...";
                m_statusTime = std::chrono::steady_clock::now();
                onDeviceChanged();
            }
        }

        // Auto-connect secondary device slot when a new device appears
        if (!curSerial.empty() && m_device.isServerRunning() && !m_slotConnected[1] && !isBatchActive()) {
            std::lock_guard<std::mutex> lk(m_deviceMutex);
            // Build set of serials belonging to the primary device (USB + WiFi of same phone)
            std::set<std::string> primarySerials;
            primarySerials.insert(curSerial);
            if (!m_usbToWifiSerial.empty()) primarySerials.insert(m_usbToWifiSerial);
            if (!m_wifiToUsbSerial.empty()) primarySerials.insert(m_wifiToUsbSerial);

            for (int i = 0; i < (int)m_devices.size(); i++) {
                if (primarySerials.count(m_devices[i].serial)) continue; // skip primary device (any serial)
                if (m_devices[i].state != "device") continue;
                std::string serial2 = m_devices[i].serial;
                LOG_INFO("Poll", "Auto-connecting secondary device slot: " + serial2);
                if (m_deviceSlots[1].startServer(serial2, true /*ADB forward*/)) {
                    m_slotSerial[1] = serial2;
                    m_slotStorageRoot[1] = m_deviceSlots[1].detectStoragePath();
                    m_slotVolumes[1].clear();
                    m_slotVolumes[1].push_back(m_slotStorageRoot[1]);
                    m_slotConnected[1] = true;
                    LOG_INFO("Poll", "Secondary slot connected: " + serial2 + " storage: " + m_slotStorageRoot[1]);
                    // Navigate any slot-1 panels
                    forEachAndroidPanel([&](FilePanel& p) {
                        if (p.deviceSlot == 1) {
                            p.currentPath = m_slotStorageRoot[1];
                            strcpy_s(p.pathInput, p.currentPath.c_str());
                            p.needsRefresh = true;
                        }
                    });
                } else {
                    LOG_WARN("Poll", "Failed to start server on secondary: " + serial2 + ": " + m_deviceSlots[1].lastError());
                }
                break; // only one secondary
            }
        }

        // Disconnect secondary slot if that device went offline
        if (m_slotConnected[1]) {
            bool slot1Online = false;
            {
                std::lock_guard<std::mutex> lk(m_deviceMutex);
                for (auto& d : m_devices) {
                    if (d.serial == m_slotSerial[1] && d.state == "device") { slot1Online = true; break; }
                }
            }
            if (!slot1Online) {
                LOG_INFO("Poll", "Secondary device disconnected: " + m_slotSerial[1]);
                m_slotConnected[1] = false;
                m_slotSerial[1].clear();
                m_slotStorageRoot[1].clear();
                m_slotVolumes[1].clear();
                // Move any slot-1 panels back to slot 0
                forEachAndroidPanel([&](FilePanel& p) {
                    if (p.deviceSlot == 1) {
                        p.deviceSlot = 0;
                        p.currentPath = m_slotStorageRoot[0].empty() ? "/" : m_slotStorageRoot[0];
                        strcpy_s(p.pathInput, p.currentPath.c_str());
                        p.needsRefresh = true;
                    }
                });
            }
        }

        // Health check
        if (m_device.isServerRunning() && !curSerial.empty() && !m_tetheringInProgress) {
            auto sinceLast = std::chrono::steady_clock::now() - m_lastTransferActivity;
            bool cooldown = sinceLast < std::chrono::seconds(15);
            if (!isBatchActive() && !cooldown) {
                if (!m_device.verifyConnection()) {
                    LOG_WARN("Poll", "Health check PING failed - server connection lost");
                    m_statusMessage = "Server connection lost";
                    m_statusTime = std::chrono::steady_clock::now();
                    forEachAndroidPanel([](FilePanel& p) {
                        p.androidEntries.clear();
                        p.selectedIndices.clear();
                        p.needsRefresh = true;
                    });
                } else {
                    forEachAndroidPanel([](FilePanel& p) {
                        if (p.androidEntries.empty()) p.needsRefresh = true;
                    });
                }
            }
        }

        // Try to reconnect saved WiFi devices
        if (!isBatchActive() && m_prefs.wifiAutoConnect) {
            bool hasUsbDevice = !curSerial.empty() && !isWifiSerial(curSerial);
            bool noDeviceAtAll = !m_device.isServerRunning();

            for (auto& w : m_prefs.savedWifiDevices) {
                if (!w.autoConnect || w.wifiIp.empty()) continue;
                if (hasUsbDevice && w.serial != curSerial) continue;
                if (!noDeviceAtAll && !hasUsbDevice) continue;
                std::string connectAddr = w.wifiIp + ":" + std::to_string(w.port);

                // Check if this WiFi device is online or offline
                bool isOnline = false;
                bool isOffline = false;
                {
                    std::lock_guard<std::mutex> lk(m_deviceMutex);
                    for (auto& d : m_devices) {
                        if (d.serial == connectAddr) {
                            if (d.state == "device") isOnline = true;
                            else isOffline = true; // "offline", "unauthorized", etc.
                            break;
                        }
                    }
                }

                // If offline or not in list at all, try to reconnect
                // Quick TCP probe first (1s timeout) to avoid 21s adb connect timeout
                if (!isOnline) {
                    SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                    if (probe != INVALID_SOCKET) {
                        unsigned long nonBlock = 1;
                        ioctlsocket(probe, FIONBIO, &nonBlock);
                        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(w.port);
                        inet_pton(AF_INET, w.wifiIp.c_str(), &addr.sin_addr);
                        connect(probe, (sockaddr*)&addr, sizeof(addr));
                        fd_set wset; FD_ZERO(&wset); FD_SET(probe, &wset);
                        timeval tv = {1, 0}; // 1 second timeout
                        int sel = select((int)probe + 1, nullptr, &wset, nullptr, &tv);
                        closesocket(probe);
                        if (sel <= 0) continue; // not reachable, skip adb connect
                    }

                    LOG_DEBUG("Poll", "WiFi device reachable, trying adb connect: " + connectAddr);
                    std::string result = m_device.runAdbCommand("connect " + connectAddr);
                    if (result.find("connected") != std::string::npos &&
                        result.find("failed") == std::string::npos) {
                        LOG_INFO("Poll", "WiFi device reconnected: " + connectAddr);
                        m_statusMessage = "WiFi reconnected: " + w.wifiIp;
                        m_statusTime = std::chrono::steady_clock::now();

                        // Re-establish secondary channel if it was lost
                        if (!m_dualChannelAvailable && m_device.isServerRunning()) {
                            m_secondaryChannel.setAdbPath(m_device.getAdbPath());
                            if (m_secondaryChannel.connectTcp(w.wifiIp, AFM_PORT) &&
                                m_secondaryChannel.verifyConnection()) {
                                m_dualChannelAvailable = true;
                                m_secondaryChannelType = "WiFi";
                                LOG_INFO("Poll", "Secondary WiFi channel re-established");
                                m_statusMessage = "Dual channel restored: USB + WiFi";
                                m_statusTime = std::chrono::steady_clock::now();
                            }
                        }
                    }
                }
            }
        }
        m_pollBusy = false;
    };

    // Main loop: use track-devices for instant notifications, fallback to polling
    while (!m_shutdownPoll) {
        if (m_device.getAdbPath().empty()) {
            m_statusMessage = "ADB not found - install Android SDK Platform Tools";
            m_statusTime = std::chrono::steady_clock::now();
            for (int i = 0; i < 50 && !m_shutdownPoll; i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Try to open track-devices for instant device notifications
        uintptr_t trackSock = m_device.openTrackDevices();

        if (trackSock != (uintptr_t)(~(uintptr_t)0)) {
            // track-devices connected — receive instant updates
            LOG_INFO("Poll", "Using ADB track-devices (instant notifications)");

            // Set a timeout on the tracking socket so we can periodically check for shutdown
            // and run health checks
            DWORD trackTimeout = 10000; // 10s — wake up periodically for health checks
            setsockopt((SOCKET)trackSock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&trackTimeout, sizeof(trackTimeout));

            while (!m_shutdownPoll) {
                auto devices = m_device.readTrackDevicesUpdate(trackSock);
                int err = WSAGetLastError();
                if (devices.empty() && (err == WSAETIMEDOUT || err == 0)) {
                    // Timeout or empty device list — run health checks with current state
                    std::string curSerial;
                    {
                        std::lock_guard<std::mutex> lk(m_deviceMutex);
                        curSerial = (m_selectedDevice >= 0 && m_selectedDevice < (int)m_devices.size()
                            && m_devices[m_selectedDevice].state == "device")
                            ? m_devices[m_selectedDevice].serial : "";
                    }
                    // If err == 0, this was a real "all devices disconnected" event
                    if (err == 0 && !m_lastDeviceSerial.empty()) {
                        processDeviceUpdate({}); // process as empty device list
                    }
                    if (m_device.isServerRunning() && !curSerial.empty() && !m_tetheringInProgress) {
                        auto sinceLast = std::chrono::steady_clock::now() - m_lastTransferActivity;
                        if (!isBatchActive() && sinceLast >= std::chrono::seconds(15)) {
                            if (!m_device.verifyConnection()) {
                                LOG_WARN("Poll", "Health check failed");
                                m_statusMessage = "Server connection lost";
                                m_statusTime = std::chrono::steady_clock::now();
                                forEachAndroidPanel([](FilePanel& p) {
                                    p.androidEntries.clear();
                                    p.selectedIndices.clear();
                                    p.needsRefresh = true;
                                });
                            } else {
                                forEachAndroidPanel([](FilePanel& p) {
                                    if (p.androidEntries.empty()) p.needsRefresh = true;
                                });
                            }
                        }
                    }
                    if (!curSerial.empty() && !m_device.isServerRunning() && !isBatchActive()) {
                        if (!isBatchActive()) {
                            LOG_INFO("Poll", "Device present but server not running - retrying");
                            m_statusMessage = "Retrying server connection...";
                            m_statusTime = std::chrono::steady_clock::now();
                            onDeviceChanged();
                        }
                    }

                    // Probe saved WiFi devices on a backoff schedule
                    // Schedule: every 5s for first 10 attempts, every 30s for next 20, every 5min after that
                    if (!m_device.isServerRunning() && !isBatchActive()) {
                        auto now = std::chrono::steady_clock::now();

                        int intervalSec;
                        if (m_wifiProbeAttempts < 10) intervalSec = 5;
                        else if (m_wifiProbeAttempts < 30) intervalSec = 30;
                        else intervalSec = 300;

                        if (now - m_lastWifiProbeTime >= std::chrono::seconds(intervalSec)) {
                            m_lastWifiProbeTime = now;
                            bool anyAutoConnect = false;
                            for (auto& w : m_prefs.savedWifiDevices) {
                                if (!w.autoConnect || w.wifiIp.empty()) continue;
                                anyAutoConnect = true;
                                std::string connectAddr = w.wifiIp + ":" + std::to_string(w.port);

                                // Quick TCP probe (1s timeout)
                                bool reachable = false;
                                SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                                if (probe != INVALID_SOCKET) {
                                    unsigned long nonBlock = 1;
                                    ioctlsocket(probe, FIONBIO, &nonBlock);
                                    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(w.port);
                                    inet_pton(AF_INET, w.wifiIp.c_str(), &addr.sin_addr);
                                    connect(probe, (sockaddr*)&addr, sizeof(addr));
                                    fd_set wset; FD_ZERO(&wset); FD_SET(probe, &wset);
                                    timeval tv = {1, 0};
                                    int sel = select((int)probe + 1, nullptr, &wset, nullptr, &tv);
                                    closesocket(probe);
                                    reachable = (sel > 0);
                                }

                                if (reachable) {
                                    LOG_INFO("Poll", "WiFi probe: " + connectAddr + " reachable, connecting...");
                                    std::string result = m_device.runAdbCommand("connect " + connectAddr);
                                    if (result.find("connected") != std::string::npos &&
                                        result.find("failed") == std::string::npos) {
                                        LOG_INFO("Poll", "WiFi auto-connect on launch: " + connectAddr);
                                        m_statusMessage = "WiFi connected: " + (w.model.empty() ? connectAddr : w.model);
                                        m_statusTime = std::chrono::steady_clock::now();
                                        m_wifiProbeAttempts = 0; // reset on success
                                        break; // connected, stop probing
                                    }
                                }
                            }
                            if (anyAutoConnect) {
                                m_wifiProbeAttempts++;
                                if (m_wifiProbeAttempts == 1)
                                    LOG_INFO("Poll", "WiFi auto-connect: probing saved devices...");
                                else if (m_wifiProbeAttempts == 10)
                                    LOG_INFO("Poll", "WiFi auto-connect: slowing to 30s interval");
                                else if (m_wifiProbeAttempts == 30)
                                    LOG_INFO("Poll", "WiFi auto-connect: slowing to 5min interval");
                            }
                        }
                    }
                    continue;
                }

                if (devices.empty()) {
                    // Real connection error — fall out to reconnect
                    LOG_WARN("Poll", "track-devices connection lost (WSA " + std::to_string(err) + ")");
                    break;
                }

                // Got a real device update — debounce by checking if the device set actually changed
                {
                    // Build a sorted set of "device"-state serials to detect actual changes
                    std::vector<std::string> onlineSerials;
                    for (auto& d : devices)
                        if (d.state == "device") onlineSerials.push_back(d.serial);
                    std::sort(onlineSerials.begin(), onlineSerials.end());

                    static std::vector<std::string> lastOnlineSerials;
                    if (onlineSerials == lastOnlineSerials) {
                        // Same set of devices — skip redundant processing
                        continue;
                    }
                    lastOnlineSerials = onlineSerials;

                    std::string firstSerial = onlineSerials.empty() ? "" : onlineSerials[0];
                    LOG_INFO("Poll", "track-devices: " + std::to_string(devices.size()) + " device(s), active serial: '" + firstSerial + "'");
                }
                processDeviceUpdate(devices);
            }

            m_device.closeTrackDevices(trackSock);

            if (m_shutdownPoll) return;
            // Brief pause before reconnecting
            for (int i = 0; i < 20 && !m_shutdownPoll; i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } else {
            // track-devices failed — fallback to polling with getDevices()
            LOG_WARN("Poll", "track-devices unavailable — falling back to polling");

            for (int cycle = 0; cycle < 3 && !m_shutdownPoll; cycle++) {
                for (int i = 0; i < 30 && !m_shutdownPoll; i++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (m_shutdownPoll) return;

                auto devices = m_device.getDevices();
                processDeviceUpdate(devices);
            }
            // After a few poll cycles, try track-devices again
        }
    }
}

void App::onDeviceChanged() {
    std::string serial;
    {
        std::lock_guard<std::mutex> lk(m_deviceMutex);
        if (m_selectedDevice < 0 || m_selectedDevice >= (int)m_devices.size()) return;
        serial = m_devices[m_selectedDevice].serial;
    }

    // Start the TCP server on the device (status updates come from m_device.statusText())
    if (!m_device.startServer(serial, m_preferAdbForward)) {
        LOG_ERROR("Poll", "startServer failed: " + m_device.lastError());
        m_statusMessage = "Server failed: " + m_device.lastError();
        m_statusTime = std::chrono::steady_clock::now();
        return;
    }
    LOG_INFO("Poll", "startServer succeeded");

    std::string connMode = m_device.isDirectConnection()
        ? ("Direct TCP @ " + m_device.deviceIp()) : "ADB Forward";
    m_statusMessage = "Connected via " + connMode + " - detecting storage...";
    m_statusTime = std::chrono::steady_clock::now();

    // Detect storage root via TCP server
    m_slotStorageRoot[0] = m_device.detectStoragePath();
    m_androidStorageRoot = m_slotStorageRoot[0];
    m_slotVolumes[0].clear();
    m_slotVolumes[0].push_back(m_slotStorageRoot[0]);
    m_androidVolumes = m_slotVolumes[0];

    // Track slot 0 — if the new primary was previously in slot 1, clear slot 1
    std::string oldSlot1Serial;
    if (m_slotConnected[1] && m_slotSerial[1] == serial) {
        LOG_INFO("Poll", "New primary was in slot 1, clearing slot 1");
        m_slotConnected[1] = false;
        oldSlot1Serial.clear(); // it's now the primary, no need to reassign
        m_slotSerial[1].clear();
        m_slotStorageRoot[1].clear();
        m_slotVolumes[1].clear();
    }
    // Remember old slot 0 serial so we can move it to slot 1
    std::string oldSlot0Serial;
    if (m_slotConnected[0] && m_slotSerial[0] != serial)
        oldSlot0Serial = m_slotSerial[0];

    m_slotSerial[0] = serial;
    m_slotConnected[0] = true;

    // Navigate slot-0 Android panels to the detected storage root
    forEachAndroidPanel([&](FilePanel& p) {
        if (p.deviceSlot == 0) {
            p.currentPath = m_androidStorageRoot;
            strcpy_s(p.pathInput, p.currentPath.c_str());
            p.needsRefresh = true;
        }
    });

    // Try to connect a second device if available
    {
        std::lock_guard<std::mutex> lk(m_deviceMutex);
        // Build set of serials belonging to the primary device (USB + WiFi of same phone)
        std::set<std::string> primarySerials;
        primarySerials.insert(serial);
        if (!m_usbToWifiSerial.empty()) primarySerials.insert(m_usbToWifiSerial);
        if (!m_wifiToUsbSerial.empty()) primarySerials.insert(m_wifiToUsbSerial);

        // Prefer reassigning the old primary to slot 1 if it's still online
        std::string candidateSerial;
        if (!oldSlot0Serial.empty() && !primarySerials.count(oldSlot0Serial)) {
            for (auto& d : m_devices) {
                if (d.serial == oldSlot0Serial && d.state == "device") {
                    candidateSerial = oldSlot0Serial;
                    break;
                }
            }
        }
        // Otherwise find any other online device
        if (candidateSerial.empty()) {
            for (int di = 0; di < (int)m_devices.size(); di++) {
                if (primarySerials.count(m_devices[di].serial)) continue;
                if (m_devices[di].state != "device") continue;
                candidateSerial = m_devices[di].serial;
                break;
            }
        }

        if (!candidateSerial.empty() && (!m_slotConnected[1] || m_slotSerial[1] != candidateSerial)) {
            LOG_INFO("Poll", "Starting server on second device: " + candidateSerial);
            if (m_deviceSlots[1].startServer(candidateSerial, true /*ADB forward*/)) {
                m_slotSerial[1] = candidateSerial;
                m_slotStorageRoot[1] = m_deviceSlots[1].detectStoragePath();
                m_slotVolumes[1].clear();
                m_slotVolumes[1].push_back(m_slotStorageRoot[1]);
                m_slotConnected[1] = true;
                LOG_INFO("Poll", "Second device connected: " + candidateSerial + " storage: " + m_slotStorageRoot[1]);

                // Navigate slot-1 Android panels
                forEachAndroidPanel([&](FilePanel& p) {
                    if (p.deviceSlot == 1) {
                        p.currentPath = m_slotStorageRoot[1];
                        strcpy_s(p.pathInput, p.currentPath.c_str());
                        p.needsRefresh = true;
                    }
                });
            } else {
                LOG_WARN("Poll", "Failed to start server on " + candidateSerial + ": " + m_deviceSlots[1].lastError());
            }
        }
    }

    // Auto-connect WiFi for saved devices (enables dual-channel automatically)
    tryAutoWifiConnect(serial);

    // Establish parallel transfer channels based on pipe count settings
    m_dualChannelAvailable = false;
    m_secondaryChannelType.clear();
    m_extraChannels.clear();
    m_activeChannelCount = 1; // primary always exists

    // Detect WiFi IP — from wlan0 query, or from WiFi serial
    std::string wifiIp;
    bool primaryIsWifi = isWifiSerial(serial);
    if (primaryIsWifi) {
        // Extract IP from WiFi serial (192.168.x.x:5555 → 192.168.x.x)
        auto colon = serial.rfind(':');
        if (colon != std::string::npos) wifiIp = serial.substr(0, colon);
    } else {
        // USB connected — try to detect WiFi IP for dual-link
        std::string wlanOut = m_device.runAdbCommand("-s " + serial + " shell \"ip -4 addr show wlan0 2>/dev/null\"");
        auto inetPos = wlanOut.find("inet ");
        if (inetPos != std::string::npos) {
            auto start = inetPos + 5;
            auto slash = wlanOut.find('/', start);
            if (slash != std::string::npos) wifiIp = wlanOut.substr(start, slash - start);
        }
    }

    // Helpers for creating channels
    int nextPort = AFM_PORT + 2; // 5742, 5743, ...
    auto makeAdbPipe = [&](DeviceClient& ch) -> bool {
        ch.setAdbPath(m_device.getAdbPath());
        ch.setLocalPort(nextPort);
        std::string fwd = m_device.runAdbCommand("-s " + serial + " forward tcp:" +
            std::to_string(nextPort) + " tcp:" + std::to_string(AFM_PORT));
        if (fwd.find("error") != std::string::npos) return false;
        bool ok = ch.connectTcp("127.0.0.1", nextPort) && ch.verifyConnection();
        nextPort++;
        return ok;
    };
    auto makeWifiDirectPipe = [&](DeviceClient& ch) -> bool {
        if (wifiIp.empty()) return false;
        ch.setAdbPath(m_device.getAdbPath());
        return ch.connectTcp(wifiIp, AFM_PORT) && ch.verifyConnection();
    };

    // Determine what pipes to create based on connection type and settings
    // USB primary: USB dual-pipe uses ADB forward, WiFi pipes use direct TCP
    // WiFi primary: WiFi dual-pipe uses direct TCP, USB pipes only if USB serial exists
    if (primaryIsWifi) {
        // WiFi-connected device: create WiFi direct pipes for dual
        int wifiPipes = m_prefs.wifiPipeCount;
        if (wifiPipes >= 2) {
            m_secondaryChannel.disconnectTcp();
            if (makeWifiDirectPipe(m_secondaryChannel)) {
                m_dualChannelAvailable = true;
                m_secondaryChannelType = "WiFi";
                m_activeChannelCount = 2;
                LOG_INFO("Poll", "Channel 2: WiFi (Direct) — dual WiFi pipe");
            }
        }
    } else {
        // USB-connected device: prefer WiFi for secondary (true dual-link), else USB dual-pipe
        bool hasWifi = !wifiIp.empty();
        if (hasWifi && m_prefs.wifiPipeCount >= 1) {
            m_secondaryChannel.disconnectTcp();
            if (makeWifiDirectPipe(m_secondaryChannel)) {
                m_dualChannelAvailable = true;
                m_secondaryChannelType = "WiFi";
                m_activeChannelCount = 2;
                LOG_INFO("Poll", "Channel 2: WiFi (Direct)");
            }
        }
        if (!m_dualChannelAvailable && m_prefs.usbPipeCount >= 2) {
            m_secondaryChannel.disconnectTcp();
            if (makeAdbPipe(m_secondaryChannel)) {
                m_dualChannelAvailable = true;
                m_secondaryChannelType = "USB";
                m_activeChannelCount = 2;
                LOG_INFO("Poll", "Channel 2: USB (ADB forward port " + std::to_string(nextPort - 1) + ")");
            }
        }
    }

    // Extra channels (3rd, 4th) for dual-pipe combinations
    if (m_dualChannelAvailable) {
        // Extra USB ADB pipe (only useful for USB-connected devices)
        if (!primaryIsWifi && m_prefs.usbPipeCount >= 2 && m_secondaryChannelType == "WiFi") {
            auto ch = std::make_unique<DeviceClient>();
            if (makeAdbPipe(*ch)) {
                LOG_INFO("Poll", "Channel " + std::to_string(m_activeChannelCount + 1) + ": USB (ADB forward)");
                m_extraChannels.push_back({std::move(ch), true});
                m_activeChannelCount++;
            }
        }
        // Extra WiFi direct pipe (when wifiPipeCount=2 and secondary is already WiFi)
        if (!wifiIp.empty() && m_prefs.wifiPipeCount >= 2) {
            auto ch = std::make_unique<DeviceClient>();
            if (makeWifiDirectPipe(*ch)) {
                LOG_INFO("Poll", "Channel " + std::to_string(m_activeChannelCount + 1) + ": WiFi (Direct)");
                m_extraChannels.push_back({std::move(ch), false});
                m_activeChannelCount++;
            }
        }
    }

    if (m_activeChannelCount > 1) {
        std::string desc;
        if (m_secondaryChannelType == "USB" && m_extraChannels.empty())
            desc = "USB (dual)";
        else if (wifiIp.empty())
            desc = "USB (" + std::to_string(m_activeChannelCount) + " pipes)";
        else
            desc = std::to_string(m_activeChannelCount) + " channels (USB + WiFi)";
        LOG_INFO("Poll", "Multi-channel ready: " + desc);
    }

    std::string channelInfo;
    if (m_activeChannelCount >= 4)
        channelInfo = " | " + std::to_string(m_activeChannelCount) + " channels";
    else if (m_activeChannelCount >= 2)
        channelInfo = m_dualChannelAvailable ? (" + " + m_secondaryChannelType) : "";
    m_statusMessage = "Ready | " + connMode + channelInfo + " | Storage: " + m_androidStorageRoot;
    m_statusTime = std::chrono::steady_clock::now();
}

// Recursively add directory contents for pull (Android → Windows)
static void addPullDirRecursive(DeviceClient& dev, const std::string& srcDir, const std::string& dstDir,
                                 std::vector<BatchFileItem>& items, uint64_t& totalBytes) {
    auto entries = dev.listDirectory(srcDir);
    for (auto& e : entries) {
        BatchFileItem sub;
        sub.displayName = e.name;
        sub.isDirectory = e.isDirectory();
        sub.fileSize = e.size;
        sub.sourcePath = srcDir + "/" + e.name;
        sub.destPath = dstDir + "\\" + e.name;
        totalBytes += sub.fileSize;
        items.push_back(sub);
        if (sub.isDirectory)
            addPullDirRecursive(dev, sub.sourcePath, sub.destPath, items, totalBytes);
    }
}

// Recursively add directory contents for push (Windows → Android)
static void addPushDirRecursive(const std::string& srcDir, const std::string& dstDir,
                                 std::vector<BatchFileItem>& items, uint64_t& totalBytes) {
    try {
        for (auto& entry : std::filesystem::directory_iterator(toFsPath(srcDir), std::filesystem::directory_options::skip_permission_denied)) {
            BatchFileItem sub;
            sub.displayName = pathToUtf8(entry.path().filename());
            sub.isDirectory = entry.is_directory();
            sub.sourcePath = pathToUtf8(entry.path());
            sub.destPath = dstDir + "/" + sub.displayName;
            if (!sub.isDirectory) {
                try { sub.fileSize = (uint64_t)entry.file_size(); } catch (...) {}
            }
            totalBytes += sub.fileSize;
            items.push_back(sub);
            if (sub.isDirectory)
                addPushDirRecursive(sub.sourcePath, sub.destPath, items, totalBytes);
        }
    } catch (...) {}
}

static void buildBatchItems(FilePanel& srcPanel, FilePanel& dstPanel, bool isPull,
                            std::vector<BatchFileItem>& items, uint64_t& totalBytes,
                            DeviceClient* dev = nullptr) {
    for (int idx : srcPanel.selectedIndices) {
        if (!srcPanel.validIndex(idx)) continue;
        BatchFileItem item;
        item.displayName = srcPanel.entryName(idx);
        item.isDirectory = srcPanel.entryIsDir(idx);
        item.fileSize = srcPanel.entrySize(idx);

        // MCRAW virtual item
        if (srcPanel.insideMcraw) {
            item.isMcrawVirtual = true;
            item.mcrawPath = srcPanel.mcrawFilePath;
            item.virtualName = item.displayName;
            item.isDirectory = false;
            if (isPull) {
                item.sourcePath = srcPanel.mcrawFilePath;
                std::string lp = dstPanel.currentPath;
                if (lp.back() != '\\') lp += "\\"; lp += item.displayName;
                item.destPath = lp;
            } else {
                item.sourcePath = srcPanel.mcrawFilePath;
                std::string rp = dstPanel.currentPath;
                if (rp.back() != '/') rp += "/"; rp += item.displayName;
                item.destPath = rp;
            }
            totalBytes += item.fileSize;
            items.push_back(std::move(item));
            continue;
        }

        if (isPull) {
            std::string rp = srcPanel.currentPath;
            if (rp.back() != '/') rp += "/"; rp += item.displayName;
            item.sourcePath = rp;
            std::string lp = dstPanel.currentPath;
            if (lp.back() != '\\') lp += "\\"; lp += item.displayName;
            item.destPath = lp;
        } else {
            std::string lp = srcPanel.currentPath;
            if (lp.back() != '\\') lp += "\\"; lp += item.displayName;
            item.sourcePath = lp;
            std::string rp = dstPanel.currentPath;
            if (rp.back() != '/') rp += "/"; rp += item.displayName;
            item.destPath = rp;
            if (!item.isDirectory) {
                try { uint64_t ls = (uint64_t)std::filesystem::file_size(toFsPath(lp)); if (ls > 0) item.fileSize = ls; } catch (...) {}
            }
        }
        totalBytes += item.fileSize;
        items.push_back(item);

        // Recursively add directory contents
        if (item.isDirectory && dev) {
            if (isPull)
                addPullDirRecursive(*dev, item.sourcePath, item.destPath, items, totalBytes);
            else
                addPushDirRecursive(item.sourcePath, item.destPath, items, totalBytes);
        }
    }
}

// Build batch items for local (Windows-to-Windows) copy
// Build batch items for cross-device Android-to-Android transfer
static void buildCrossDeviceBatchItems(FilePanel& srcPanel, FilePanel& dstPanel,
                                        std::vector<BatchFileItem>& items, uint64_t& totalBytes) {
    for (int idx : srcPanel.selectedIndices) {
        if (!srcPanel.validIndex(idx)) continue;
        BatchFileItem item;
        item.displayName = srcPanel.entryName(idx);
        item.isDirectory = srcPanel.entryIsDir(idx);
        item.fileSize = srcPanel.entrySize(idx);

        // Both source and dest are Android — use forward slashes
        std::string sp = srcPanel.currentPath;
        if (sp.back() != '/') sp += "/"; sp += item.displayName;
        item.sourcePath = sp;

        std::string dp = dstPanel.currentPath;
        if (dp.back() != '/') dp += "/"; dp += item.displayName;
        item.destPath = dp;

        totalBytes += item.fileSize;
        items.push_back(std::move(item));
    }
}

// Recursively add local directory contents
static void addLocalDirRecursive(const std::string& srcDir, const std::string& dstDir,
                                  std::vector<BatchFileItem>& items, uint64_t& totalBytes) {
    try {
        for (auto& entry : std::filesystem::directory_iterator(toFsPath(srcDir), std::filesystem::directory_options::skip_permission_denied)) {
            BatchFileItem sub;
            sub.displayName = pathToUtf8(entry.path().filename());
            sub.isDirectory = entry.is_directory();
            sub.sourcePath = pathToUtf8(entry.path());
            sub.destPath = dstDir + "\\" + sub.displayName;
            if (!sub.isDirectory) {
                try { sub.fileSize = (uint64_t)entry.file_size(); } catch (...) {}
            }
            totalBytes += sub.fileSize;
            items.push_back(sub);
            if (sub.isDirectory)
                addLocalDirRecursive(sub.sourcePath, sub.destPath, items, totalBytes);
        }
    } catch (...) {}
}

static void buildLocalBatchItems(FilePanel& srcPanel, FilePanel& dstPanel,
                                  std::vector<BatchFileItem>& items, uint64_t& totalBytes) {
    for (int idx : srcPanel.selectedIndices) {
        if (!srcPanel.validIndex(idx)) continue;
        BatchFileItem item;
        item.displayName = srcPanel.entryName(idx);
        item.isDirectory = srcPanel.entryIsDir(idx);
        item.fileSize = srcPanel.entrySize(idx);

        std::string sp = srcPanel.currentPath;
        if (sp.back() != '\\') sp += "\\"; sp += item.displayName;
        item.sourcePath = sp;

        std::string dp = dstPanel.currentPath;
        if (dp.back() != '\\') dp += "\\"; dp += item.displayName;
        item.destPath = dp;

        if (!item.isDirectory) {
            try { uint64_t s = (uint64_t)std::filesystem::file_size(toFsPath(sp)); if (s > 0) item.fileSize = s; } catch (...) {}
        }
        totalBytes += item.fileSize;
        items.push_back(item);

        if (item.isDirectory)
            addLocalDirRecursive(item.sourcePath, item.destPath, items, totalBytes);
    }
}

void App::startTransfer(bool pullFromAndroid) {
    // Determine source and destination panels based on their modes
    FilePanel* srcPtr = nullptr;
    FilePanel* dstPtr = nullptr;
    bool localCopy = false;

    bool bothWindows = !m_leftPanel.isAndroid && !m_rightPanel.isAndroid;
    bool bothAndroid = m_leftPanel.isAndroid && m_rightPanel.isAndroid;

    if (bothWindows) {
        // Windows-to-Windows: use focused panel as source, other as destination
        srcPtr = (m_lastFocusedPanel == &m_rightPanel) ? &m_rightPanel : &m_leftPanel;
        dstPtr = (srcPtr == &m_leftPanel) ? &m_rightPanel : &m_leftPanel;
        localCopy = true;
    } else if (bothAndroid) {
        // Android-to-Android: need two different devices
        if (m_leftPanel.deviceSlot == m_rightPanel.deviceSlot) {
            m_statusMessage = "Both panels are on the same device — use different devices";
            m_statusTime = std::chrono::steady_clock::now();
            return;
        }
        srcPtr = (m_lastFocusedPanel == &m_rightPanel) ? &m_rightPanel : &m_leftPanel;
        dstPtr = (srcPtr == &m_leftPanel) ? &m_rightPanel : &m_leftPanel;
    } else {
        // Mixed: need device
        {
            std::lock_guard<std::mutex> lk(m_deviceMutex);
            if (m_selectedDevice < 0 || m_devices.empty()) return;
        }
        if (pullFromAndroid) {
            srcPtr = m_leftPanel.isAndroid ? &m_leftPanel : &m_rightPanel;
            dstPtr = !m_leftPanel.isAndroid ? &m_leftPanel : &m_rightPanel;
        } else {
            srcPtr = !m_leftPanel.isAndroid ? &m_leftPanel : &m_rightPanel;
            dstPtr = m_leftPanel.isAndroid ? &m_leftPanel : &m_rightPanel;
        }
    }

    FilePanel& src = *srcPtr;
    FilePanel& dst = *dstPtr;
    if (src.selectedIndices.empty()) {
        m_statusMessage = "No files selected for transfer";
        m_statusTime = std::chrono::steady_clock::now();
        return;
    }

    auto batch = std::make_shared<TransferBatch>();
    batch->isLocalCopy = localCopy;
    batch->srcDeviceSlot = src.deviceSlot;
    batch->dstDeviceSlot = dst.deviceSlot;
    batch->useParallelChannels = m_dualChannelAvailable;

    // Cross-device Android-to-Android
    if (bothAndroid && src.deviceSlot != dst.deviceSlot) {
        batch->isCrossDevice = true;
        batch->isPull = false;
    } else {
        batch->isPull = !localCopy && pullFromAndroid;
    }

    uint64_t total = 0;
    if (localCopy)
        buildLocalBatchItems(src, dst, batch->files, total);
    else if (batch->isCrossDevice)
        buildCrossDeviceBatchItems(src, dst, batch->files, total);
    else
        buildBatchItems(src, dst, batch->isPull, batch->files, total, &deviceFor(src));
    batch->totalBytes = total;

    if (batch->files.empty()) return;

    if (batch->isLocalCopy) {
        // Same drive → auto-move (instant rename), cross-drive → ask copy/move
        bool sameDrive = false;
        if (!batch->files.empty()) {
            auto& s = batch->files[0].sourcePath;
            auto& d = batch->files[0].destPath;
            if (s.size() >= 2 && d.size() >= 2)
                sameDrive = (toupper(s[0]) == toupper(d[0]) && s[1] == ':' && d[1] == ':');
        }
        if (sameDrive) {
            batch->isMove = true;
            std::lock_guard<std::mutex> lk(m_batchMutex);
            m_batchQueue.push_back(batch);
            m_batchCV.notify_one();
            m_overlayWasOpen = false;
        } else {
            m_pendingBatch = batch;
            m_showCopyMoveDialog = true;
        }
    } else if (batch->isCrossDevice) {
        m_pendingBatch = batch;
        m_showCrossDeviceDialog = true;
    } else {
        std::lock_guard<std::mutex> lk(m_batchMutex);
        m_batchQueue.push_back(batch);
        m_batchCV.notify_one();
        m_overlayWasOpen = false;
    }
}

void App::handleExternalFileDrop(const std::vector<std::string>& paths, int mouseX, int mouseY) {
    if (paths.empty()) return;

    // Determine which panel the drop landed on
    float mx = (float)mouseX, my = (float)mouseY;
    FilePanel* target = nullptr;
    if (mx >= m_leftPanelMin.x && mx <= m_leftPanelMax.x &&
        my >= m_leftPanelMin.y && my <= m_leftPanelMax.y) {
        target = &m_leftPanel;
    } else if (mx >= m_rightPanelMin.x && mx <= m_rightPanelMax.x &&
               my >= m_rightPanelMin.y && my <= m_rightPanelMax.y) {
        target = &m_rightPanel;
    }

    if (!target) {
        m_statusMessage = "Drop outside panel area";
        m_statusTime = std::chrono::steady_clock::now();
        return;
    }

    // Build batch items from dropped paths
    auto batch = std::make_shared<TransferBatch>();
    uint64_t total = 0;

    for (auto& srcPath : paths) {
        BatchFileItem item;
        auto p = toFsPath(srcPath);
        item.displayName = pathToUtf8(p.filename());
        item.sourcePath = srcPath;
        item.isDirectory = std::filesystem::is_directory(p);

        if (target->isAndroid) {
            // Explorer → Android: push
            std::string rp = target->currentPath;
            if (rp.back() != '/') rp += "/";
            rp += item.displayName;
            item.destPath = rp;
            batch->isPull = false;
        } else {
            // Explorer → Windows panel: local copy
            std::string dp = target->currentPath;
            if (dp.back() != '\\') dp += "\\";
            dp += item.displayName;
            item.destPath = dp;
            batch->isLocalCopy = true;
        }

        if (!item.isDirectory) {
            try { item.fileSize = (uint64_t)std::filesystem::file_size(p); } catch (...) {}
        }
        total += item.fileSize;
        batch->files.push_back(std::move(item));
    }

    batch->totalBytes = total;
    if (batch->files.empty()) return;

    if (!batch->isLocalCopy) {
        std::lock_guard<std::mutex> lk(m_deviceMutex);
        if (m_selectedDevice < 0 || m_devices.empty()) {
            m_statusMessage = "No device connected for transfer";
            m_statusTime = std::chrono::steady_clock::now();
            return;
        }
    }

    if (batch->isLocalCopy) {
        bool sameDrive = false;
        if (!batch->files.empty()) {
            auto& s = batch->files[0].sourcePath;
            auto& d = batch->files[0].destPath;
            if (s.size() >= 2 && d.size() >= 2)
                sameDrive = (toupper(s[0]) == toupper(d[0]) && s[1] == ':' && d[1] == ':');
        }
        if (sameDrive) {
            batch->isMove = true;
            std::lock_guard<std::mutex> lk(m_batchMutex);
            m_batchQueue.push_back(batch);
            m_batchCV.notify_one();
            m_overlayWasOpen = false;
        } else {
            m_pendingBatch = batch;
            m_showCopyMoveDialog = true;
        }
    } else {
        std::lock_guard<std::mutex> lk(m_batchMutex);
        m_batchQueue.push_back(batch);
        m_batchCV.notify_one();
        m_overlayWasOpen = false;
    }
}

void App::startTransferFromDrag(FilePanel& srcPanel, FilePanel& dstPanel) {
    if (srcPanel.selectedIndices.empty()) return;

    bool localCopy = !srcPanel.isAndroid && !dstPanel.isAndroid;
    bool isPull = srcPanel.isAndroid;

    if (!localCopy) {
        std::lock_guard<std::mutex> lk(m_deviceMutex);
        if (m_selectedDevice < 0 || m_devices.empty()) return;
    }

    auto batch = std::make_shared<TransferBatch>();
    batch->isLocalCopy = localCopy;
    batch->srcDeviceSlot = srcPanel.deviceSlot;
    batch->useParallelChannels = m_dualChannelAvailable;
    batch->dstDeviceSlot = dstPanel.deviceSlot;

    // Cross-device Android-to-Android via drag
    bool crossDevice = srcPanel.isAndroid && dstPanel.isAndroid && srcPanel.deviceSlot != dstPanel.deviceSlot;
    if (crossDevice) {
        batch->isCrossDevice = true;
        batch->isPull = false;
    } else {
        batch->isPull = !localCopy && isPull;
    }

    uint64_t total = 0;
    if (localCopy)
        buildLocalBatchItems(srcPanel, dstPanel, batch->files, total);
    else if (crossDevice)
        buildCrossDeviceBatchItems(srcPanel, dstPanel, batch->files, total);
    else
        buildBatchItems(srcPanel, dstPanel, isPull, batch->files, total, &deviceFor(srcPanel));
    batch->totalBytes = total;

    if (batch->files.empty()) return;

    if (batch->isLocalCopy) {
        bool sameDrive = false;
        if (!batch->files.empty()) {
            auto& s = batch->files[0].sourcePath;
            auto& d = batch->files[0].destPath;
            if (s.size() >= 2 && d.size() >= 2)
                sameDrive = (toupper(s[0]) == toupper(d[0]) && s[1] == ':' && d[1] == ':');
        }
        if (sameDrive) {
            batch->isMove = true;
            std::lock_guard<std::mutex> lk(m_batchMutex);
            m_batchQueue.push_back(batch);
            m_batchCV.notify_one();
            m_overlayWasOpen = false;
        } else {
            m_pendingBatch = batch;
            m_showCopyMoveDialog = true;
        }
    } else if (batch->isCrossDevice) {
        m_pendingBatch = batch;
        m_showCrossDeviceDialog = true;
    } else {
        std::lock_guard<std::mutex> lk(m_batchMutex);
        m_batchQueue.push_back(batch);
        m_batchCV.notify_one();
        m_overlayWasOpen = false;
    }
}

void App::processBatchQueue() {
    // Lower thread priority so transfers don't starve UI and other apps
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    // Set low I/O priority so disk reads don't evict other apps' cache
    THREAD_POWER_THROTTLING_STATE throttle{};
    throttle.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    throttle.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    throttle.StateMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &throttle, sizeof(throttle));

    while (!m_shutdownTransfer) {
        std::shared_ptr<TransferBatch> batch;
        {
            std::unique_lock<std::mutex> lk(m_batchMutex);
            m_batchCV.wait(lk, [&]() {
                if (m_shutdownTransfer) return true;
                for (auto& b : m_batchQueue)
                    if (b->state.load() == BatchState::Queued) return true;
                return false;
            });
            if (m_shutdownTransfer) return;
            for (auto& b : m_batchQueue) {
                if (b->state.load() == BatchState::Queued) { batch = b; break; }
            }
            while (m_batchQueue.size() > 10) {
                BatchState s = m_batchQueue.front()->state.load();
                if (s == BatchState::Completed || s == BatchState::Failed || s == BatchState::Stopped)
                    m_batchQueue.pop_front();
                else break;
            }
        }
        if (!batch) continue;

        // Get the device client for this batch
        DeviceClient& batchDev = m_deviceSlots[batch->srcDeviceSlot];

        if (!batch->isLocalCopy && !batch->isCrossDevice && !batchDev.isServerRunning()) {
            batch->state = BatchState::Failed;
            batch->errorMessage = "Server not connected";
            continue;
        }

        // Remember the original device serial for reconnection safety
        std::string originalSerial = batch->isLocalCopy ? "" : batchDev.connectedSerial();

        batch->state = BatchState::Running;
        batch->startTime = std::chrono::steady_clock::now();

        // --- Multi-NIC channel setup ---
        bool useMultiNic = false;
        std::vector<DeviceClient*> multiNicDevPtrs;
        std::vector<std::string> multiNicBindIps;
        std::vector<std::string> multiNicNames;
        std::string multiNicDeviceIp;

        if (m_prefs.enableMultiNic && !batch->isLocalCopy && !batch->isCrossDevice) {
            std::vector<NicBinding> activeNics;
            for (auto& nb : m_prefs.multiNicBindings)
                if (nb.enabled) activeNics.push_back(nb);

            if (activeNics.size() >= 2) {
                // Find the device's WiFi IP from any available source
                multiNicDeviceIp = batchDev.isDirectConnection() ? batchDev.deviceIp() : "";
                if (multiNicDeviceIp.empty()) {
                    // Try ADB to get wlan0 IP
                    std::string serial = batchDev.connectedSerial();
                    std::string wlanOut = m_device.runAdbCommand(
                        "-s " + serial + " shell \"ip -4 addr show wlan0 2>/dev/null\"");
                    auto inetPos = wlanOut.find("inet ");
                    if (inetPos != std::string::npos) {
                        auto start = inetPos + 5;
                        auto slash = wlanOut.find('/', start);
                        if (slash != std::string::npos)
                            multiNicDeviceIp = wlanOut.substr(start, slash - start);
                    }
                    // Fallback: saved WiFi devices
                    if (multiNicDeviceIp.empty()) {
                        for (auto& w : m_prefs.savedWifiDevices) {
                            if (w.serial == serial && !w.wifiIp.empty()) {
                                multiNicDeviceIp = w.wifiIp;
                                break;
                            }
                        }
                    }
                }
                if (!multiNicDeviceIp.empty()) {
                    // Detect USB tethering IP to label USB channels correctly
                    std::string usbTetheringIp;
                    {
                        std::string serial = batchDev.connectedSerial();
                        usbTetheringIp = batchDev.detectDeviceIp(serial);
                    }

                    m_nicChannels.clear();
                    m_nicLocalIps.clear();
                    for (int i = 0; i < (int)activeNics.size(); i++) {
                        auto ch = std::make_unique<DeviceClient>();
                        if (ch->connectTcp(multiNicDeviceIp, AFM_PORT, activeNics[i].localIp) &&
                            ch->verifyConnection()) {
                            multiNicDevPtrs.push_back(ch.get());
                            multiNicBindIps.push_back(activeNics[i].localIp);
                            // Label USB tethering adapters as "USB" (same /24 subnet as tethering IP)
                            std::string name = activeNics[i].adapterName;
                            if (!usbTetheringIp.empty()) {
                                auto lastDot1 = usbTetheringIp.rfind('.');
                                auto lastDot2 = activeNics[i].localIp.rfind('.');
                                if (lastDot1 != std::string::npos && lastDot2 != std::string::npos &&
                                    usbTetheringIp.substr(0, lastDot1) == activeNics[i].localIp.substr(0, lastDot2))
                                    name = "USB";
                            }
                            multiNicNames.push_back(name);
                            m_nicChannels.push_back(std::move(ch));
                            LOG_INFO("MultiNIC", "Channel " + std::to_string(i) + " via " +
                                     name + " (" + activeNics[i].localIp + ")");
                        } else {
                            LOG_WARN("MultiNIC", "Channel failed via " + activeNics[i].adapterName +
                                     " (" + activeNics[i].localIp + "): " + ch->lastError());
                        }
                    }
                    if ((int)multiNicDevPtrs.size() >= 2) {
                        useMultiNic = true;
                        batch->useMultiNic = true;
                        batch->numChannels = (int)multiNicDevPtrs.size();
                        LOG_INFO("MultiNIC", std::to_string(multiNicDevPtrs.size()) + " channels ready");
                    } else {
                        for (auto& ch : m_nicChannels) ch->disconnectTcp();
                        m_nicChannels.clear();
                        m_nicLocalIps.clear();
                        LOG_WARN("MultiNIC", "Not enough channels connected, falling back");
                    }
                }
            }
        }

        // --- Parallel transfer (dual-channel or multi-NIC) ---
        bool doDualChannel = batch->useParallelChannels && m_dualChannelAvailable &&
            m_secondaryChannel.isServerRunning() && !useMultiNic;
        if ((doDualChannel || useMultiNic) && !batch->isLocalCopy &&
            !batch->isCrossDevice && batch->totalFiles() >= 1) {
            LOG_INFO("Transfer", useMultiNic ?
                "Starting multi-NIC parallel transfer (" + std::to_string(batch->numChannels) +
                " channels, " + std::to_string(batch->totalFiles()) + " files)" :
                "Starting parallel dual-channel transfer (" +
                std::to_string(batch->totalFiles()) + " files)");

            auto parallelStart = std::chrono::steady_clock::now();

            // Setup per-channel names
            if (useMultiNic) {
                for (int i = 0; i < (int)multiNicDevPtrs.size(); i++)
                    batch->channels[i].channelName = multiNicNames[i];
            } else {
                std::string primarySerial = batchDev.connectedSerial();
                bool usbCableConnected = !isWifiSerial(primarySerial);
                batch->channels[0].channelName = usbCableConnected ? "USB" : "WiFi (ADB)";
                // Determine actual secondary type — verify USB device is still online
                bool secIsUsb = (m_secondaryChannelType == "USB");
                if (secIsUsb && !usbCableConnected) {
                    bool usbOnline = false;
                    std::lock_guard<std::mutex> lk(m_deviceMutex);
                    for (auto& d : m_devices)
                        if (d.state == "device" && !isWifiSerial(d.serial)) { usbOnline = true; break; }
                    if (!usbOnline) secIsUsb = false;
                }
                batch->channels[1].channelName = secIsUsb ? "USB (ADB)" : "WiFi (Direct)";
            }

            // === Work-stealing block queue for file distribution ===
            // Small files (<200MB): go into a shared whole-file queue
            // Large files (>=200MB): split into 200MB blocks, go into a shared block queue
            // Both channels pull from the queue — faster channel naturally gets more work

            // Scale block size by file size: larger files get bigger blocks to reduce
            // per-block overhead at high transfer speeds (600+ MB/s)
            static const uint64_t MB = 1024ULL * 1024;
            auto blockSizeFor = [](uint64_t fileSize) -> uint64_t {
                const uint64_t MB = 1024ULL * 1024;
                const uint64_t GB = 1024ULL * MB;
                if (fileSize >= 10 * GB) return 2 * GB;   // 30GB file → 15 blocks instead of 150
                if (fileSize >= 2 * GB)  return 1 * GB;   // 5GB file → 5 blocks instead of 25
                if (fileSize >= 500 * MB) return 500 * MB; // 1GB file → 2 blocks instead of 5
                return 200 * MB;                           // small files → 200MB blocks
            };

            struct WorkBlock {
                int fileIndex;
                uint64_t offset;    // byte offset within file (0 for whole files)
                uint64_t length;    // bytes to transfer (fileSize for whole files)
                bool isWholeFile;   // true = use pullFile, false = use readRangeStreaming
                int blockId;        // sequential ID for CRC ordering
                int retries = 0;    // how many times this block has been requeued after failure
            };

            // --- File conflict check for parallel transfers ---
            for (int fi = 0; fi < batch->totalFiles() && !m_shutdownTransfer; fi++) {
                if (batch->stopRequested.load()) break;
                auto& item = batch->files[fi];
                if (item.isDirectory) continue;

                bool destExists = false;
                if (batch->isPull) {
                    destExists = std::filesystem::exists(toFsPath(item.destPath));
                } else {
                    DeviceClient& destDev = batchDev;
                    uint64_t remoteSize = destDev.getFileSize(item.destPath);
                    destExists = (remoteSize > 0);
                }

                if (destExists) {
                    ConflictAction action = batch->conflictAllDecision;
                    if (action == ConflictAction::None) {
                        batch->conflictFileName = item.displayName;
                        batch->waitingConflict = true;
                        batch->conflictResponse = ConflictAction::None;
                        batch->state = BatchState::WaitingConflict;
                        {
                            std::unique_lock<std::mutex> lk(m_batchMutex);
                            m_batchCV.wait(lk, [&]() {
                                return m_shutdownTransfer || batch->stopRequested.load() ||
                                       batch->conflictResponse.load() != ConflictAction::None;
                            });
                        }
                        batch->waitingConflict = false;
                        action = batch->conflictResponse.load();
                        batch->conflictResponse = ConflictAction::None;
                        batch->state = BatchState::Running;

                        if (action == ConflictAction::OverwriteAll) {
                            batch->conflictAllDecision = ConflictAction::OverwriteAll;
                            action = ConflictAction::Overwrite;
                        } else if (action == ConflictAction::SkipAll) {
                            batch->conflictAllDecision = ConflictAction::SkipAll;
                            action = ConflictAction::Skip;
                        }
                    } else {
                        if (action == ConflictAction::OverwriteAll) action = ConflictAction::Overwrite;
                        else if (action == ConflictAction::SkipAll) action = ConflictAction::Skip;
                    }

                    if (batch->stopRequested.load() || m_shutdownTransfer) break;

                    if (action == ConflictAction::Skip) {
                        batch->skippedFiles++;
                        // Remove this file from the batch so it's not queued
                        item.fileSize = 0;
                        item.isDirectory = true; // mark as dir so workers skip it
                        continue;
                    }
                }
            }
            if (batch->stopRequested.load()) { batch->state = BatchState::Stopped; continue; }

            // Recalculate total after skips
            { uint64_t newTotal = 0; for (auto& f : batch->files) newTotal += f.fileSize; batch->totalBytes = newTotal; }

            // Build the work queue
            std::deque<WorkBlock> workQueue;
            std::mutex workMutex;
            int blockIdCounter = 0;

            // Track per-file block completion for partial cleanup on cancel
            std::vector<int> totalBlocksPerFile(batch->totalFiles(), 0);
            std::vector<int> completedBlocksPerFile(batch->totalFiles(), 0);
            std::set<int> completedFileIndices;

            // Pre-create output files for split files (both channels write via OVERLAPPED)
            struct SplitFileHandle {
                int fileIndex;
                HANDLE handle;
                uint64_t fileSize;
            };
            std::vector<SplitFileHandle> splitHandles;

            // Track CRC per block for combining later
            struct BlockCrc {
                int blockId;
                uint32_t crc;
                uint64_t length;
                int fileIndex;
            };
            std::vector<BlockCrc> allBlockCrcs;
            std::mutex crcMutex;

            // Store inline CRCs for whole-file pulls (each pullFile overwrites the previous,
            // so we capture them per-file for use during verification)
            std::unordered_map<std::string, uint32_t> wholeFileCrcs;  // destPath -> CRC

            for (int i = 0; i < batch->totalFiles(); i++) {
                auto& item = batch->files[i];
                if (item.isDirectory) {
                    // Directories: just create, no transfer
                    workQueue.push_back({i, 0, 0, true, blockIdCounter++});
                    totalBlocksPerFile[i] = 1;
                } else if (item.fileSize < 200 * MB) {
                    // Small files: transfer as whole file (pushFile/pullFile streaming)
                    workQueue.push_back({i, 0, item.fileSize, true, blockIdCounter++});
                    totalBlocksPerFile[i] = 1;
                } else {
                    // Large file: split into blocks (size scales with file size)
                    uint64_t blkSize = blockSizeFor(item.fileSize);
                    uint64_t remaining = item.fileSize;
                    uint64_t offset = 0;
                    int blockCount = 0;
                    while (remaining > 0) {
                        uint64_t blockLen = std::min(remaining, blkSize);
                        workQueue.push_back({i, offset, blockLen, false, blockIdCounter++});
                        offset += blockLen;
                        remaining -= blockLen;
                        blockCount++;
                    }
                    totalBlocksPerFile[i] = blockCount;
                    if (batch->isPull) {
                        // Ensure parent directory exists before creating the split file
                        // (directory batch items haven't been processed by workers yet)
                        try { std::filesystem::create_directories(std::filesystem::path(toFsPath(item.destPath)).parent_path()); } catch (...) {}
                        // Pre-create local output file
                        HANDLE h = CreateFileW(toWide(item.destPath).c_str(), GENERIC_WRITE, FILE_SHARE_WRITE, nullptr,
                                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                        if (h != INVALID_HANDLE_VALUE) {
                            LARGE_INTEGER li; li.QuadPart = (LONGLONG)item.fileSize;
                            SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
                            SetEndOfFile(h);
                            splitHandles.push_back({i, h, item.fileSize});
                        }
                    } else {
                        // Pre-create remote file on device with pre-allocation
                        batchDev.createFile(item.destPath, item.fileSize);
                        splitHandles.push_back({i, INVALID_HANDLE_VALUE, item.fileSize});
                    }
                    LOG_INFO("Transfer", "Splitting " + item.displayName + " (" + formatSize(item.fileSize) +
                             ") into " + std::to_string(blockCount) + " x " + formatSize(blkSize) + " blocks");
                }
            }

            int totalBlocks = (int)workQueue.size();
            int numCh = useMultiNic ? batch->numChannels : 2;
            uint64_t perChBytes = batch->totalBytes.load() / numCh;
            for (int c = 0; c < numCh; c++) {
                batch->channels[c].totalBytes = perChBytes;
                batch->channels[c].filesAssigned = (c == 0) ? totalBlocks : 0;
            }

            // Worker: pull blocks from shared queue (work-stealing)
            // Store WiFi IP for reconnection (secondary channel has no serial set)
            // Use the IP that was actually used for the secondary channel, not a saved one
            std::string secondaryWifiIp;
            {
                // Extract IP from the actual secondary connection
                // The secondary channel was established to the WiFi IP of the current device
                std::string serial = batchDev.connectedSerial();
                // Try to get WiFi IP from the current device
                std::string wlanOut = m_device.runAdbCommand("-s " + serial + " shell \"ip -4 addr show wlan0 2>/dev/null\"");
                auto inetPos = wlanOut.find("inet ");
                if (inetPos != std::string::npos) {
                    auto start = inetPos + 5;
                    auto slash = wlanOut.find('/', start);
                    if (slash != std::string::npos)
                        secondaryWifiIp = wlanOut.substr(start, slash - start);
                }
                // Fallback: extract from the saved device matching this serial
                if (secondaryWifiIp.empty()) {
                    for (auto& w : m_prefs.savedWifiDevices)
                        if (w.serial == serial && !w.wifiIp.empty()) { secondaryWifiIp = w.wifiIp; break; }
                }
                if (!secondaryWifiIp.empty())
                    LOG_INFO("Transfer", "Secondary WiFi reconnect IP: " + secondaryWifiIp);
            }

            // Capture the device's hardware serial (ro.serialno) so reconnect can verify
            // it's the same device, not a different one that also has a server running
            std::string deviceHwSerial;
            {
                std::string serial = batchDev.connectedSerial();
                if (serial.empty()) {
                    // Try any available serial for the query
                    std::string devList = m_device.runAdbCommand("devices");
                    std::istringstream diss(devList);
                    std::string dline;
                    while (std::getline(diss, dline))
                        if (dline.find("\tdevice") != std::string::npos) {
                            auto tab = dline.find('\t');
                            if (tab != std::string::npos) { serial = dline.substr(0, tab); break; }
                        }
                }
                if (!serial.empty()) {
                    std::string hwOut = m_device.runAdbCommand("-s " + serial + " shell getprop ro.serialno");
                    // Trim whitespace
                    while (!hwOut.empty() && (hwOut.back() == '\n' || hwOut.back() == '\r' || hwOut.back() == ' '))
                        hwOut.pop_back();
                    deviceHwSerial = hwOut;
                    LOG_INFO("Transfer", "Device hardware serial: " + deviceHwSerial);
                }
            }

            // (usbChannelCount removed — each channel independently restores its preferred transport)

            auto worker = [&, deviceHwSerial](DeviceClient& dev, TransferBatch::ChannelProgress& ch,
                              const std::string& reconnectIp, const std::string& bindLocalIp,
                              bool preferUsb) {
                auto chStart = std::chrono::steady_clock::now();
                uint64_t chBytesCompleted = 0;
                uint64_t lastSpeedBytes = 0;
                auto lastSpeedTime = chStart;
                // Capture original serial and whether this channel uses ADB forward at start,
                // so reconnect logic uses the correct transport even if serial auto-switches
                std::string origSerial = dev.connectedSerial();
                bool origIsAdbForward = !dev.isDirectConnection();
                // Channel 0 (primary) prefers USB, channel 1 (secondary) prefers WiFi
                bool origIsUsb = preferUsb;
                // Determine ACTUAL current physical link from the connection state
                bool currentIsUsb = !origSerial.empty() && !isWifiSerial(origSerial);
                if (!currentIsUsb && origIsAdbForward) {
                    // ADB forward through WiFi serial = WiFi link, through USB serial = USB link
                    currentIsUsb = !origSerial.empty() && !isWifiSerial(origSerial);
                }
                auto lastUpgradeCheck = std::chrono::steady_clock::now();

                while (!m_shutdownTransfer && !batch->stopRequested.load()) {
                    // Every 5s, check if we can restore the original transport for true dual-channel.
                    // This catches: USB dropped → both on WiFi → USB comes back → upgrade one channel.
                    // Also: WiFi dropped → both on USB → WiFi comes back → upgrade one channel.
                    {
                        auto now = std::chrono::steady_clock::now();
                        if (std::chrono::duration<double>(now - lastUpgradeCheck).count() >= 5.0) {
                            lastUpgradeCheck = now;
                            // Only upgrade if we're NOT on our original physical link
                            // AND both channels are on the same link (otherwise already dual-channel)
                            if (currentIsUsb != origIsUsb) {
                                bool upgraded = false;
                                if (origIsUsb) {
                                    // Originally USB, now on WiFi. Try to get back to USB.
                                    // Find USB serial for this device
                                    std::string devList = dev.runAdbCommand("devices");
                                    std::istringstream iss(devList);
                                    std::string line;
                                    while (std::getline(iss, line)) {
                                        if (line.find("\tdevice") == std::string::npos) continue;
                                        auto tab = line.find('\t');
                                        if (tab == std::string::npos) continue;
                                        std::string s = line.substr(0, tab);
                                        if (s.empty() || isWifiSerial(s)) continue;
                                        // Verify it's our device
                                        if (!deviceHwSerial.empty()) {
                                            std::string hw = dev.runAdbCommand("-s " + s + " shell getprop ro.serialno");
                                            while (!hw.empty() && (hw.back() == '\n' || hw.back() == '\r' || hw.back() == ' ')) hw.pop_back();
                                            if (hw != deviceHwSerial) continue;
                                        }
                                        std::string fwd = dev.runAdbCommand("-s " + s + " forward tcp:" +
                                            std::to_string(dev.localPort()) + " tcp:" + std::to_string(AFM_PORT));
                                        if (fwd.find("error") == std::string::npos && fwd.find("offline") == std::string::npos) {
                                            dev.disconnectTcp();
                                            if (dev.connectTcp("127.0.0.1", dev.localPort()) && dev.verifyConnection()) {
                                                ch.channelName = "USB (ADB)";
                                                LOG_INFO("Transfer", ch.channelName + " upgraded back to USB");
                                                upgraded = true;
                                            }
                                        }
                                        break;
                                    }
                                } else {
                                    // Originally WiFi, now on USB. Try to get back to WiFi.
                                    // IMPORTANT: probe with a throwaway socket first so we don't
                                    // kill the working USB connection if WiFi is still down.
                                    std::string ip = reconnectIp;
                                    if (ip.empty() && !origSerial.empty()) {
                                        auto colon = origSerial.rfind(':');
                                        if (colon != std::string::npos) ip = origSerial.substr(0, colon);
                                    }
                                    if (!ip.empty()) {
                                        // Quick TCP probe without disrupting current connection
                                        SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                                        if (probe != INVALID_SOCKET) {
                                            u_long nonBlock = 1;
                                            ioctlsocket(probe, FIONBIO, &nonBlock);
                                            struct sockaddr_in addr = {};
                                            addr.sin_family = AF_INET;
                                            addr.sin_port = htons(AFM_PORT);
                                            inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
                                            connect(probe, (struct sockaddr*)&addr, sizeof(addr));
                                            fd_set wset;
                                            FD_ZERO(&wset);
                                            FD_SET(probe, &wset);
                                            struct timeval tv = {1, 0}; // 1 second timeout
                                            bool reachable = (select((int)probe + 1, nullptr, &wset, nullptr, &tv) > 0);
                                            closesocket(probe);
                                            if (reachable) {
                                                // WiFi is reachable, now switch for real
                                                dev.disconnectTcp();
                                                if (dev.connectTcp(ip, AFM_PORT) && dev.verifyConnection()) {
                                                    ch.channelName = "WiFi (Direct)";
                                                    LOG_INFO("Transfer", ch.channelName + " upgraded back to WiFi");
                                                    upgraded = true;
                                                }
                                            }
                                        }
                                    }
                                }
                                if (upgraded) {
                                    bool wasUsb = currentIsUsb;
                                    currentIsUsb = origIsUsb;
                                    ch.speed = 0;
                                    lastSpeedBytes = ch.bytesTransferred.load();
                                    lastSpeedTime = std::chrono::steady_clock::now();
                                }
                            }
                        }
                    }

                    // Grab next work block
                    WorkBlock block;
                    {
                        std::lock_guard<std::mutex> lk(workMutex);
                        if (workQueue.empty()) break;
                        block = workQueue.front();
                        workQueue.pop_front();
                    }

                    auto& item = batch->files[block.fileIndex];
                    if (item.isDirectory) {
                        if (batch->isPull)
                            try { std::filesystem::create_directories(item.destPath); } catch (...) {}
                        else
                            dev.createDirectory(item.destPath);
                        ch.filesCompleted++;
                        continue;
                    }

                    ch.curBlockSize = block.length;
                    ch.curBlockTransferred = 0;
                    if (block.isWholeFile)
                        ch.currentFile = item.displayName;
                    else
                        ch.currentFile = item.displayName + " [block " + formatSize(block.offset) + "]";

                    // Progress callback (shared by whole-file and block transfers)
                    auto updateProgress = [&](uint64_t blockTransferred) {
                        ch.bytesTransferred = chBytesCompleted + blockTransferred;
                        ch.curBlockTransferred = blockTransferred;
                        // Speed
                        auto now = std::chrono::steady_clock::now();
                        double dt = std::chrono::duration<double>(now - lastSpeedTime).count();
                        if (dt >= 0.5) {
                            double delta = (double)(ch.bytesTransferred.load() - lastSpeedBytes);
                            double instant = delta / dt;
                            // Cap instant speed to 10 GB/s to avoid overflow spikes during recovery
                            if (instant > 10e9) instant = 0;
                            if (instant < 0) instant = 0;
                            ch.speed = (ch.speed.load() > 0) ? ch.speed.load() * 0.8 + instant * 0.2 : instant;
                            lastSpeedBytes = ch.bytesTransferred.load();
                            lastSpeedTime = now;
                        }
                        // Combined progress across all channels
                        uint64_t combined = 0;
                        double combinedSpeed = 0;
                        for (int c = 0; c < numCh; c++) {
                            combined += batch->channels[c].bytesTransferred.load();
                            combinedSpeed += batch->channels[c].speed.load();
                        }
                        batch->totalTransferred = combined;
                        uint64_t tb = batch->totalBytes.load();
                        if (tb > 0) batch->totalProgress = (float)((double)combined / (double)tb);
                        batch->speedBytesPerSec = combinedSpeed;
                        batch->etaSeconds = (combinedSpeed > 0 && tb > combined)
                            ? (double)(tb - combined) / combinedSpeed : -1.0;
                    };

                    auto progressCb = [&](uint64_t transferred, uint64_t total) -> bool {
                        if (m_shutdownTransfer || batch->stopRequested.load()) return false;
                        while (batch->pauseRequested.load() && !m_shutdownTransfer && !batch->stopRequested.load()) {
                            batch->state = BatchState::Paused;
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        }
                        if (m_shutdownTransfer || batch->stopRequested.load()) return false;
                        batch->state = BatchState::Running;
                        updateProgress(transferred);
                        return true;
                    };

                    bool ok = false;

                    if (block.isWholeFile) {
                        // Whole file transfer
                        uint64_t outSize = 0;
                        if (batch->isPull) {
                            ok = dev.pullFile(item.sourcePath, item.destPath, outSize, progressCb);
                            // Capture inline CRC before next pullFile overwrites it
                            if (ok && !batch->stopRequested.load()) {
                                std::lock_guard<std::mutex> lk(crcMutex);
                                wholeFileCrcs[item.destPath] = dev.getInlineCrc();
                            }
                        } else {
                            ok = dev.pushFile(item.sourcePath, item.destPath, item.fileSize, progressCb);
                            // Capture inline CRC (computed from local source file during push)
                            if (ok && !batch->stopRequested.load()) {
                                std::lock_guard<std::mutex> lk(crcMutex);
                                wholeFileCrcs[item.sourcePath] = dev.getInlineCrc();
                            }
                        }
                    } else if (batch->isPull) {
                        // Pull block: read range from device, write to local file
                        uint32_t blockCrc = ~(uint32_t)0;
                        HANDLE fh = INVALID_HANDLE_VALUE;
                        for (auto& sh : splitHandles)
                            if (sh.fileIndex == block.fileIndex) { fh = sh.handle; break; }

                        uint64_t blockReceived = 0;
                        auto rangeCb = [&](const void* data, uint32_t len, uint64_t offset) -> bool {
                            if (m_shutdownTransfer || batch->stopRequested.load()) return false;
                            while (batch->pauseRequested.load() && !m_shutdownTransfer && !batch->stopRequested.load()) {
                                batch->state = BatchState::Paused;
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            }
                            if (m_shutdownTransfer || batch->stopRequested.load()) return false;
                            batch->state = BatchState::Running;
                            crc32Update(blockCrc, data, len);
                            if (fh != INVALID_HANDLE_VALUE) {
                                OVERLAPPED ov = {};
                                ov.Offset = (DWORD)(offset & 0xFFFFFFFF);
                                ov.OffsetHigh = (DWORD)(offset >> 32);
                                DWORD written;
                                WriteFile(fh, data, len, &written, &ov);
                            }
                            blockReceived += len;
                            updateProgress(blockReceived);
                            return true;
                        };

                        uint64_t got = dev.readRangeStreaming(item.sourcePath, block.offset, block.length, rangeCb);
                        ok = (got >= block.length) || batch->stopRequested.load();
                        if (ok && !batch->stopRequested.load()) {
                            std::lock_guard<std::mutex> lk(crcMutex);
                            allBlockCrcs.push_back({block.blockId, ~blockCrc, block.length, block.fileIndex});
                        }
                    } else {
                        // Push block: stream local file range to device via single writeRangeStreaming call
                        uint64_t written = dev.writeRangeStreaming(
                            item.destPath, block.offset,
                            item.sourcePath, block.offset, block.length,
                            progressCb);
                        ok = (written >= block.length) || batch->stopRequested.load();
                        // Compute block CRC from local file for verification (avoids re-reading later)
                        if (ok && !batch->stopRequested.load()) {
                            uint32_t blockCrc = ~(uint32_t)0;
                            HANDLE hCrc = CreateFileW(toWide(item.sourcePath).c_str(), GENERIC_READ,
                                FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
                            if (hCrc != INVALID_HANDLE_VALUE) {
                                LARGE_INTEGER seekPos; seekPos.QuadPart = (LONGLONG)block.offset;
                                SetFilePointerEx(hCrc, seekPos, nullptr, FILE_BEGIN);
                                char crcBuf[256 * 1024];
                                uint64_t remaining = block.length;
                                while (remaining > 0) {
                                    DWORD toRead = (DWORD)std::min(remaining, (uint64_t)sizeof(crcBuf));
                                    DWORD bytesRead = 0;
                                    if (!ReadFile(hCrc, crcBuf, toRead, &bytesRead, nullptr) || bytesRead == 0) break;
                                    crc32Update(blockCrc, crcBuf, bytesRead);
                                    remaining -= bytesRead;
                                }
                                CloseHandle(hCrc);
                                std::lock_guard<std::mutex> lk(crcMutex);
                                allBlockCrcs.push_back({block.blockId, ~blockCrc, block.length, block.fileIndex});
                            }
                        }
                    }

                    if (!ok && !batch->stopRequested.load()) {
                        std::string errMsg = dev.lastError();

                        // Check if this is a file-level error (server responded properly)
                        // vs an actual connection failure. File errors like "No such file or
                        // directory" or "Permission denied" mean the connection is still alive,
                        // we just need to skip this file and move on.
                        bool isFileError = (errMsg.find("No such file") != std::string::npos ||
                                            errMsg.find("Permission denied") != std::string::npos ||
                                            errMsg.find("Is a directory") != std::string::npos ||
                                            errMsg.find("Not a directory") != std::string::npos);

                        if (isFileError) {
                            LOG_WARN("Transfer", ch.channelName + " skipping: " + item.displayName +
                                     " - " + errMsg);
                            batch->errorSkippedFiles++;
                            {
                                std::lock_guard<std::mutex> lk(batch->errorSkippedMutex);
                                // Avoid duplicate entries (split files may skip multiple blocks)
                                auto& names = batch->errorSkippedNames;
                                if (names.empty() || names.back() != item.displayName)
                                    names.push_back(item.displayName);
                            }
                            // Mark the block as completed so the file doesn't stall the transfer
                            chBytesCompleted += block.length;
                            ch.bytesTransferred = chBytesCompleted;
                            ch.filesCompleted++;
                            {
                                std::lock_guard<std::mutex> lk(workMutex);
                                completedBlocksPerFile[block.fileIndex]++;
                                if (completedBlocksPerFile[block.fileIndex] >= totalBlocksPerFile[block.fileIndex])
                                    completedFileIndices.insert(block.fileIndex);
                            }
                            continue; // grab next block from queue
                        }

                        block.retries++;
                        if (block.retries >= 3) {
                            // Too many retries for this block, skip it
                            LOG_WARN("Transfer", ch.channelName + " giving up after " +
                                     std::to_string(block.retries) + " retries: " + item.displayName);
                            batch->errorSkippedFiles++;
                            {
                                std::lock_guard<std::mutex> lk(batch->errorSkippedMutex);
                                auto& names = batch->errorSkippedNames;
                                if (std::find(names.begin(), names.end(), item.displayName) == names.end())
                                    names.push_back(item.displayName);
                            }
                            chBytesCompleted += block.length;
                            ch.bytesTransferred = chBytesCompleted;
                            ch.filesCompleted++;
                            {
                                std::lock_guard<std::mutex> lk(workMutex);
                                completedBlocksPerFile[block.fileIndex]++;
                                if (completedBlocksPerFile[block.fileIndex] >= totalBlocksPerFile[block.fileIndex])
                                    completedFileIndices.insert(block.fileIndex);
                            }
                            continue;
                        }

                        LOG_WARN("Transfer", ch.channelName + " channel lost: " + item.displayName +
                                 " - " + errMsg);
                        ch.speed = 0;
                        ch.currentFile = "(reconnecting...)";

                        // Return failed block to queue so other channel can grab it meanwhile
                        {
                            std::lock_guard<std::mutex> lk(workMutex);
                            workQueue.push_back(block);
                        }

                        // Try to reconnect just the TCP socket (NOT restart the server)
                        // Keep trying until transfer ends or stop is requested
                        bool recovered = false;
                        // Use the serial captured at worker start to reconnect to the same transport
                        std::string serial = origSerial;
                        for (int retry = 1; !batch->stopRequested.load(); retry++) {
                            // Check if there are still blocks left to process
                            {
                                std::lock_guard<std::mutex> lk(workMutex);
                                if (workQueue.empty()) break; // no work left, no point reconnecting
                            }
                            ch.currentFile = "(reconnecting #" + std::to_string(retry) + ")";
                            LOG_INFO("Transfer", ch.channelName + " reconnect attempt " + std::to_string(retry));
                            std::this_thread::sleep_for(std::chrono::seconds(3));

                            // Disconnect old socket
                            dev.disconnectTcp();

                            bool connected = false;
                            std::string actualTransport = ch.channelName; // default to current name
                            if (!bindLocalIp.empty()) {
                                // Multi-NIC channel: reconnect with same NIC binding
                                connected = dev.connectTcp(reconnectIp, AFM_PORT, bindLocalIp) && dev.verifyConnection();
                            } else {
                                // Universal reconnect: try all available transports
                                // 1. Try original transport first
                                if (origIsAdbForward) {
                                    // Try ADB forward first (original transport)
                                    // ... then fall back to WiFi direct
                                } else {
                                    // Try WiFi direct first (original transport)
                                    std::string ip;
                                    if (!serial.empty()) {
                                        auto colon = serial.rfind(':');
                                        ip = (colon != std::string::npos) ? serial.substr(0, colon) : serial;
                                    } else if (!reconnectIp.empty()) {
                                        ip = reconnectIp;
                                    }
                                    if (!ip.empty()) {
                                        connected = dev.connectTcp(ip, AFM_PORT) && dev.verifyConnection();
                                        if (connected) actualTransport = "WiFi (Direct)";
                                    }
                                }

                                // 2. Try ADB forward (works for both channel types as fallback)
                                if (!connected) {
                                    std::vector<std::string> candidates;
                                    if (!serial.empty()) candidates.push_back(serial);
                                    {
                                        std::string devList = dev.runAdbCommand("devices");
                                        std::istringstream iss(devList);
                                        std::string line;
                                        while (std::getline(iss, line)) {
                                            if (line.find("\tdevice") != std::string::npos) {
                                                auto tab = line.find('\t');
                                                if (tab != std::string::npos) {
                                                    std::string s = line.substr(0, tab);
                                                    if (!s.empty() && s != serial) candidates.push_back(s);
                                                }
                                            }
                                        }
                                    }
                                    for (auto& cand : candidates) {
                                        if (!deviceHwSerial.empty()) {
                                            std::string hwCheck = dev.runAdbCommand("-s " + cand + " shell getprop ro.serialno");
                                            while (!hwCheck.empty() && (hwCheck.back() == '\n' || hwCheck.back() == '\r' || hwCheck.back() == ' '))
                                                hwCheck.pop_back();
                                            if (hwCheck != deviceHwSerial) continue;
                                        }
                                        std::string fwdResult = dev.runAdbCommand("-s " + cand + " forward tcp:" +
                                            std::to_string(dev.localPort()) + " tcp:" + std::to_string(AFM_PORT));
                                        if (fwdResult.find("error") == std::string::npos &&
                                            fwdResult.find("offline") == std::string::npos) {
                                            connected = dev.connectTcp("127.0.0.1", dev.localPort()) && dev.verifyConnection();
                                            if (connected) {
                                                actualTransport = isWifiSerial(cand) ? "WiFi (ADB)" : "USB (ADB)";
                                                LOG_INFO("Transfer", "Reconnected via ADB forward to serial: " + cand);
                                                break;
                                            }
                                        }
                                    }
                                }

                                // 3. If ADB-forward channel, also try WiFi direct as last resort
                                if (!connected && origIsAdbForward && !reconnectIp.empty()) {
                                    connected = dev.connectTcp(reconnectIp, AFM_PORT) && dev.verifyConnection();
                                    if (connected) actualTransport = "WiFi (Direct)";
                                }
                            }

                            if (connected) {
                                // Update channel name to reflect actual transport used
                                if (!bindLocalIp.empty()) {
                                    // Multi-NIC: keep original name
                                } else {
                                    ch.channelName = actualTransport;
                                }
                                // Track which physical link we're on for upgrade checks
                                currentIsUsb = (actualTransport == "USB (ADB)");
                                LOG_INFO("Transfer", ch.channelName + " reconnected - resuming");
                                recovered = true;
                                ch.speed = 0;
                                lastSpeedBytes = ch.bytesTransferred.load();
                                lastSpeedTime = std::chrono::steady_clock::now();
                                break;
                            }
                        }

                        if (!recovered) {
                            ch.currentFile = "(disconnected)";
                            LOG_INFO("Transfer", ch.channelName + " no more blocks - stopping reconnect");
                            break;
                        }
                        continue; // grab next block from queue
                    }

                    chBytesCompleted += block.length;
                    ch.bytesTransferred = chBytesCompleted;
                    ch.filesCompleted++;

                    // Track per-file completion for partial cleanup on cancel
                    {
                        std::lock_guard<std::mutex> lk(workMutex);
                        completedBlocksPerFile[block.fileIndex]++;
                        if (completedBlocksPerFile[block.fileIndex] >= totalBlocksPerFile[block.fileIndex])
                            completedFileIndices.insert(block.fileIndex);
                    }

                    if (block.isWholeFile)
                        LOG_INFO("Transfer", ch.channelName + " done: " + item.displayName);
                    else
                        LOG_INFO("Transfer", ch.channelName + " block done: " + item.displayName +
                                 " [" + formatSize(block.offset) + "+" + formatSize(block.length) + "]");
                }

                ch.speed = 0; // zero speed when channel exits (so combined speed is accurate)
                ch.elapsedSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - chStart).count();
            };

            if (useMultiNic) {
                std::vector<std::thread> threads;
                for (int i = 0; i < (int)multiNicDevPtrs.size(); i++) {
                    threads.emplace_back(worker, std::ref(*multiNicDevPtrs[i]),
                                         std::ref(batch->channels[i]),
                                         multiNicDeviceIp, multiNicBindIps[i], true);
                }
                for (auto& t : threads) t.join();
            } else {
                // Launch all available channels as worker threads
                std::vector<std::thread> threads;
                batch->numChannels = std::min(m_activeChannelCount, (int)TransferBatch::MAX_CHANNELS);
                numCh = batch->numChannels;
                // Channel 0: primary (batchDev) — prefer based on actual connection
                bool primaryIsUsb = !batchDev.connectedSerial().empty() &&
                                    !isWifiSerial(batchDev.connectedSerial());
                batch->channels[0].channelName = primaryIsUsb ? "USB (ADB)" : "WiFi (ADB)";
                threads.emplace_back(worker, std::ref(batchDev), std::ref(batch->channels[0]),
                                     secondaryWifiIp, std::string(""), primaryIsUsb);
                // Channel 1: secondary — prefers WiFi if available
                if (batch->numChannels >= 2) {
                    batch->channels[1].channelName = (m_secondaryChannelType == "WiFi") ? "WiFi (Direct)" : "USB (ADB)";
                    threads.emplace_back(worker, std::ref(m_secondaryChannel), std::ref(batch->channels[1]),
                                         secondaryWifiIp, std::string(""), m_secondaryChannelType != "WiFi");
                }
                // Channels 2+: extra pipes
                for (int i = 0; i < (int)m_extraChannels.size() && (i + 2) < batch->numChannels; i++) {
                    bool extraIsUsb = m_extraChannels[i].isUsb;
                    std::string chName = extraIsUsb ? "USB (ADB)" : "WiFi (Direct)";
                    batch->channels[i + 2].channelName = chName;
                    threads.emplace_back(worker, std::ref(*m_extraChannels[i].dev), std::ref(batch->channels[i + 2]),
                                         secondaryWifiIp, std::string(""), extraIsUsb);
                }
                for (auto& t : threads) t.join();
            }

            // Close all split file handles
            for (auto& sh : splitHandles) {
                if (sh.handle != INVALID_HANDLE_VALUE) CloseHandle(sh.handle);
            }

            // --- CRC verification for parallel transfers ---
            if (batch->state.load() != BatchState::Failed && !batch->stopRequested.load() &&
                m_prefs.enableCrcVerification) {
                batch->state = BatchState::Verifying;

                // Combine block CRCs for split files — sort by blockId and chain crc32Combine
                if (!allBlockCrcs.empty()) {
                    std::sort(allBlockCrcs.begin(), allBlockCrcs.end(),
                              [](auto& a, auto& b) { return a.blockId < b.blockId; });

                    // Group by file and combine
                    std::unordered_map<int, std::vector<BlockCrc*>> fileBlocks;
                    for (auto& bc : allBlockCrcs) fileBlocks[bc.fileIndex].push_back(&bc);

                    for (auto& [fi, blocks] : fileBlocks) {
                        if (blocks.size() <= 1) continue;
                        // Chain combine: crc = combine(crc, next_block_crc, next_block_len)
                        uint32_t combined = blocks[0]->crc;
                        for (size_t i = 1; i < blocks.size(); i++)
                            combined = crc32Combine(combined, blocks[i]->crc, blocks[i]->length);
                        auto& item = batch->files[fi];
                        std::string crcLocalPath = batch->isPull ? item.destPath : item.sourcePath;
                        wholeFileCrcs[crcLocalPath] = combined;
                        LOG_INFO("Transfer", "Block CRC combined (" + std::to_string(blocks.size()) +
                                 " blocks): " + item.displayName + " crc=" + std::to_string(combined));
                    }
                }

                for (int fi = 0; fi < batch->totalFiles(); fi++) {
                    if (batch->stopRequested.load()) break;
                    auto& item = batch->files[fi];
                    if (item.isDirectory) continue;

                    // For pulls: sourcePath=Android, destPath=Windows
                    // For pushes: sourcePath=Windows, destPath=Android
                    std::string remotePath = batch->isPull ? item.sourcePath : item.destPath;
                    std::string localPath = batch->isPull ? item.destPath : item.sourcePath;

                    batch->crcFileName = item.displayName;
                    batch->crcProgress = 0.0f;
                    batch->crcPhase = 0;
                    batch->errorMessage = "Verifying: " + item.displayName;

                    std::string crcDetail;
                    double crcRemoteMs = 0, crcLocalMs = 0;

                    // Set inline CRC from per-file map (whole files from any channel)
                    // or from block combination (split files, set above)
                    auto wfIt = wholeFileCrcs.find(localPath);
                    if (wfIt != wholeFileCrcs.end())
                        batchDev.setInlineCrc(wfIt->second, localPath);

                    bool crcOk = batchDev.verifyFileCrc(remotePath, localPath, crcDetail,
                                                        &batch->crcProgress, &batch->crcPhase,
                                                        &crcRemoteMs, &crcLocalMs);

                    TransferBatch::CrcResult crcRes;
                    crcRes.fileName = item.displayName;
                    crcRes.passed = crcOk;
                    crcRes.detail = crcDetail;
                    crcRes.remoteMs = crcRemoteMs;
                    crcRes.localMs = crcLocalMs;
                    crcRes.totalMs = crcRemoteMs + crcLocalMs;
                    crcRes.fileSize = item.fileSize;
                    crcRes.sourcePath = item.sourcePath;  // original batch paths (not CRC-rearranged)
                    crcRes.destPath = item.destPath;
                    batch->crcResults.push_back(std::move(crcRes));

                    if (!crcOk) {
                        LOG_ERROR("Transfer", "CRC FAILED (parallel): " + item.displayName + " - " + crcDetail);
                        batch->errorMessage = "CRC FAILED: " + item.displayName;
                        // Check if this was a block-split file
                        bool wasSplit = false;
                        for (auto& bc : allBlockCrcs) if (bc.fileIndex == fi) { wasSplit = true; break; }
                        if (wasSplit)
                            batch->errorMessage += " (block-split — file likely corrupted)";
                    } else {
                        LOG_INFO("Transfer", "CRC OK (parallel): " + item.displayName);
                        batch->errorMessage.clear();
                    }
                }

                batch->crcFileName.clear();
                batch->crcProgress = 0.0f;
                batch->crcPhase = 0;
            }

            // Finalize
            if (batch->state.load() != BatchState::Failed && !batch->stopRequested.load()) {
                // Check if any CRC failed
                bool anyCrcFail = false;
                for (auto& r : batch->crcResults) if (!r.passed) anyCrcFail = true;

                batch->totalProgress = 1.0f;
                batch->totalTransferred = batch->totalBytes.load();
                auto totalTime = std::chrono::steady_clock::now() - parallelStart;
                double wallSec = std::chrono::duration<double>(totalTime).count();
                if (wallSec < 0.1) wallSec = 0.1;
                double avgSpeed = (double)batch->totalTransferred.load() / wallSec;
                batch->finalAvgSpeed = avgSpeed;
                batch->finalTimeSec = wallSec;
                batch->speedBytesPerSec = 0;
                batch->etaSeconds = -1;

                if (anyCrcFail) {
                    batch->state = BatchState::Failed;
                    batch->errorMessage = "CRC verification failed — file(s) may be corrupted";
                } else {
                    batch->state = BatchState::Completed;
                }

                m_statusMessage = "Parallel done: " + std::to_string(batch->totalFiles()) + " files, " +
                    formatSize(batch->totalTransferred.load()) + " @ " + formatSpeed(avgSpeed) +
                    (useMultiNic ? " (" + std::to_string(numCh) + " NICs)" : " (dual channel)");
                m_statusTime = std::chrono::steady_clock::now();

                m_leftPanel.needsRefresh = true;
                m_rightPanel.needsRefresh = true;
            } else if (batch->stopRequested.load()) {
                batch->state = BatchState::Stopped;
            }

            // Clean up only incomplete files on stop/cancel/error
            // Files whose blocks all finished are kept; only partially-transferred files are deleted
            if (batch->state.load() == BatchState::Stopped || batch->state.load() == BatchState::Failed) {
                for (int fi = 0; fi < batch->totalFiles(); fi++) {
                    auto& item = batch->files[fi];
                    if (item.isDirectory) continue;
                    if (completedFileIndices.count(fi)) continue; // fully transferred, keep it
                    if (batch->isPull) {
                        try {
                            if (std::filesystem::exists(toFsPath(item.destPath))) {
                                std::filesystem::remove(toFsPath(item.destPath));
                                LOG_INFO("Transfer", "Deleted incomplete file: " + item.destPath);
                            }
                        } catch (...) {}
                    } else {
                        batchDev.deleteFile(item.destPath);
                        LOG_INFO("Transfer", "Deleted incomplete remote file: " + item.destPath);
                    }
                }
                if (useMultiNic) {
                    for (auto* dev : multiNicDevPtrs) {
                        dev->flushStaleData();
                        dev->restoreTimeouts();
                    }
                } else {
                    batchDev.flushStaleData();
                    batchDev.restoreTimeouts();
                    m_secondaryChannel.flushStaleData();
                    m_secondaryChannel.restoreTimeouts();
                }
            }
            // Clean up multi-NIC channels after transfer completes
            if (useMultiNic) {
                for (auto& ch : m_nicChannels) ch->disconnectTcp();
                m_nicChannels.clear();
                m_nicLocalIps.clear();
            }
            continue;
        }

        uint64_t bytesCompletedBefore = 0;
        auto batchStart = std::chrono::steady_clock::now();
        uint64_t lastSpeedBytes = 0;
        auto lastSpeedTime = batchStart;

        // --- Fast parallel MCRAW batch extraction ---
        // If this is a local copy and contains MCRAW virtual DNG frames from the same container,
        // extract the DNG frames in parallel, then handle non-DNG items (metadata, audio) normally.
        if (batch->isLocalCopy && !batch->files.empty()) {
            std::string mcrawContainer;
            int dngCount = 0;
            int nonDngCount = 0;
            std::string destDir;
            for (auto& item : batch->files) {
                if (item.isDirectory) continue;
                if (item.isMcrawVirtual && item.virtualName.find(".dng") != std::string::npos) {
                    if (mcrawContainer.empty()) {
                        mcrawContainer = item.mcrawPath;
                        auto dp = std::filesystem::path(item.destPath).parent_path();
                        destDir = dp.string();
                    } else if (item.mcrawPath != mcrawContainer) {
                        mcrawContainer.clear(); // mixed containers, can't batch
                        break;
                    }
                    dngCount++;
                } else {
                    nonDngCount++;
                }
            }

            if (dngCount >= 2 && !mcrawContainer.empty() && !destDir.empty()) {
                // Use parallel batch extraction for DNG frames
                LOG_INFO("Transfer", "MCRAW batch path: " + std::to_string(dngCount) + " DNG + " +
                         std::to_string(nonDngCount) + " other items from " + mcrawContainer);
                try { std::filesystem::create_directories(destDir); } catch (...) {}

                int extracted = extractLocalMcrawBatch(
                    mcrawContainer, destDir, 0, // auto thread count
                    &batch->stopRequested,
                    [&](int completed, int total, uint64_t bytesWritten) {
                        batch->currentFileIndex = completed - 1;
                        batch->totalTransferred = bytesWritten;
                        if (total > 0)
                            batch->totalProgress = (float)completed / (float)total;
                        batch->curFileProgress = 1.0f;

                        // Speed tracking
                        auto now = std::chrono::steady_clock::now();
                        double dt = std::chrono::duration<double>(now - lastSpeedTime).count();
                        if (dt >= 0.5) {
                            double delta = (double)(bytesWritten - lastSpeedBytes);
                            double instantSpeed = delta / dt;
                            double prevSpeed = batch->speedBytesPerSec.load();
                            double smoothed = (prevSpeed > 0) ? prevSpeed * 0.8 + instantSpeed * 0.2 : instantSpeed;
                            batch->speedBytesPerSec = smoothed;
                            uint64_t tb = batch->totalBytes.load();
                            if (smoothed > 0 && tb > bytesWritten)
                                batch->etaSeconds = (double)(tb - bytesWritten) / smoothed;
                            else batch->etaSeconds = -1.0;
                            lastSpeedBytes = bytesWritten;
                            lastSpeedTime = now;
                        }

                        m_lastTransferActivity = std::chrono::steady_clock::now();
                    });

                bytesCompletedBefore = batch->totalTransferred.load();

                if (batch->stopRequested.load()) {
                    batch->state = BatchState::Stopped;
                    continue;
                }

                // If there are non-DNG items (metadata.json, audio.wav), extract them now
                if (nonDngCount > 0) {
                    for (auto& item : batch->files) {
                        if (batch->stopRequested.load()) break;
                        if (item.isDirectory || !item.isMcrawVirtual) continue;
                        if (item.virtualName.find(".dng") != std::string::npos) continue; // already extracted
                        extractLocalMcrawItem(mcrawContainer, item.virtualName, item.destPath);
                    }
                }

                // Update final state
                batch->totalTransferred = batch->totalBytes.load();
                batch->totalProgress = 1.0f;
                if (extracted >= dngCount) {
                    batch->state = BatchState::Completed;
                } else if (extracted > 0) {
                    batch->state = BatchState::Completed;
                } else {
                    batch->state = BatchState::Failed;
                    batch->errorMessage = "MCRAW batch extraction failed";
                }
                continue; // skip per-item loop
            }
        }

        for (int fi = 0; fi < batch->totalFiles() && !m_shutdownTransfer; fi++) {
            if (batch->stopRequested.load()) { batch->state = BatchState::Stopped; break; }

            // Pause — track paused duration for accurate avg speed
            if (batch->pauseRequested.load()) {
                auto pauseStart = std::chrono::steady_clock::now();
                while (batch->pauseRequested.load() && !m_shutdownTransfer && !batch->stopRequested.load()) {
                    batch->state = BatchState::Paused;
                    std::unique_lock<std::mutex> lk(m_batchMutex);
                    m_batchCV.wait_for(lk, std::chrono::milliseconds(200));
                }
                batch->pausedSeconds = batch->pausedSeconds.load() +
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - pauseStart).count();
            }
            if (batch->stopRequested.load()) { batch->state = BatchState::Stopped; break; }
            batch->state = BatchState::Running;

            batch->currentFileIndex = fi;
            auto& item = batch->files[fi];

            // Get file size if unknown
            if (item.fileSize == 0 && !item.isDirectory) {
                if (batch->isPull) item.fileSize = batchDev.getFileSize(item.sourcePath);
                else { try { item.fileSize = std::filesystem::file_size(toFsPath(item.sourcePath)); } catch (...) {} }
                uint64_t newTotal = 0;
                for (auto& f : batch->files) newTotal += f.fileSize;
                batch->totalBytes = newTotal;
            }

            batch->curFileSize = item.fileSize;
            batch->curFileTransferred = 0;
            batch->curFileProgress = 0.0f;

            // --- Write permission pre-check for local destinations ---
            if (!item.isDirectory && (batch->isPull || batch->isLocalCopy)) {
                // Test if we can create a file in the destination directory
                std::filesystem::path destDir = toFsPath(item.destPath).parent_path();
                try {
                    std::filesystem::create_directories(destDir);
                } catch (...) {}
                std::string testPath = pathToUtf8(destDir) + "\\.afm_write_test";
                HANDLE hTest = CreateFileW(toWide(testPath).c_str(), GENERIC_WRITE, 0, nullptr,
                                           CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
                if (hTest == INVALID_HANDLE_VALUE) {
                    DWORD err = GetLastError();
                    batch->state = BatchState::Failed;
                    if (err == ERROR_ACCESS_DENIED)
                        batch->errorMessage = "Access denied: " + pathToUtf8(destDir) + "\n\nThis location requires administrator rights.";
                    else
                        batch->errorMessage = "Cannot write to: " + pathToUtf8(destDir) + " (error " + std::to_string(err) + ")";
                    break;
                }
                CloseHandle(hTest); // FILE_FLAG_DELETE_ON_CLOSE auto-deletes

                // Also check if destination file specifically is read-only
                if (std::filesystem::exists(toFsPath(item.destPath))) {
                    auto perms = std::filesystem::status(toFsPath(item.destPath)).permissions();
                    if ((perms & std::filesystem::perms::owner_write) == std::filesystem::perms::none) {
                        batch->state = BatchState::Failed;
                        batch->errorMessage = "Destination file is read-only: " + item.displayName;
                        break;
                    }
                }
            }

            // --- File conflict check ---
            if (!item.isDirectory) {
                bool destExists = false;
                if (batch->isLocalCopy || batch->isPull) {
                    // Destination is Windows — check local filesystem
                    destExists = std::filesystem::exists(toFsPath(item.destPath));
                } else {
                    // Destination is Android — use the DESTINATION device client
                    DeviceClient& destDev = batch->isCrossDevice
                        ? m_deviceSlots[batch->dstDeviceSlot & 1] : batchDev;
                    uint64_t remoteSize = destDev.getFileSize(item.destPath);
                    destExists = (remoteSize > 0);
                }

                if (destExists) {
                    ConflictAction action = batch->conflictAllDecision;

                    if (action == ConflictAction::None) {
                        // No sticky decision — ask the user
                        batch->conflictFileName = item.displayName;
                        batch->waitingConflict = true;
                        batch->conflictResponse = ConflictAction::None;
                        batch->state = BatchState::WaitingConflict;

                        // Wait for UI thread to set conflictResponse
                        {
                            std::unique_lock<std::mutex> lk(m_batchMutex);
                            m_batchCV.wait(lk, [&]() {
                                return m_shutdownTransfer || batch->stopRequested.load() ||
                                       batch->conflictResponse.load() != ConflictAction::None;
                            });
                        }
                        batch->waitingConflict = false;
                        action = batch->conflictResponse.load();
                        batch->conflictResponse = ConflictAction::None;
                        batch->state = BatchState::Running;

                        // Store sticky "All" decisions
                        if (action == ConflictAction::OverwriteAll) {
                            batch->conflictAllDecision = ConflictAction::OverwriteAll;
                            action = ConflictAction::Overwrite;
                        } else if (action == ConflictAction::SkipAll) {
                            batch->conflictAllDecision = ConflictAction::SkipAll;
                            action = ConflictAction::Skip;
                        }
                    } else {
                        // Sticky decision — map "All" variants to their single action
                        if (action == ConflictAction::OverwriteAll) action = ConflictAction::Overwrite;
                        else if (action == ConflictAction::SkipAll) action = ConflictAction::Skip;
                    }

                    if (batch->stopRequested.load() || m_shutdownTransfer) {
                        batch->state = BatchState::Stopped;
                        break;
                    }

                    if (action == ConflictAction::Skip) {
                        batch->skippedFiles++;
                        bytesCompletedBefore += item.fileSize;
                        batch->totalTransferred = bytesCompletedBefore;
                        if (batch->totalBytes.load() > 0)
                            batch->totalProgress = (float)((double)bytesCompletedBefore / (double)batch->totalBytes.load());
                        continue; // skip this file
                    }
                    // else: Overwrite — fall through to normal transfer
                }
            }

            // Progress callback - updates batch atomics, called from DeviceClient
            auto progressCb = [&](uint64_t transferred, uint64_t total) -> bool {
                // Check stop/shutdown
                if (m_shutdownTransfer || batch->stopRequested.load()) return false;

                // Handle pause inside transfer — track paused duration
                if (batch->pauseRequested.load()) {
                    auto pStart = std::chrono::steady_clock::now();
                    while (batch->pauseRequested.load() && !m_shutdownTransfer && !batch->stopRequested.load()) {
                        batch->state = BatchState::Paused;
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    batch->pausedSeconds = batch->pausedSeconds.load() +
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - pStart).count();
                }
                if (batch->stopRequested.load()) return false;
                batch->state = BatchState::Running;

                // Update per-file progress
                batch->curFileTransferred = transferred;
                if (total > 0) batch->curFileProgress = (float)((double)transferred / (double)total);

                // Update total progress
                uint64_t totalTx = bytesCompletedBefore + transferred;
                batch->totalTransferred = totalTx;
                uint64_t tb = batch->totalBytes.load();
                if (tb > 0) batch->totalProgress = (float)((double)totalTx / (double)tb);

                // Speed/ETA
                auto now = std::chrono::steady_clock::now();
                double dt = std::chrono::duration<double>(now - lastSpeedTime).count();
                if (dt >= 0.5) {
                    double delta = (double)(totalTx - lastSpeedBytes);
                    double instantSpeed = delta / dt;
                    // Exponential moving average — smooths out burst/pause fluctuations
                    double prevSpeed = batch->speedBytesPerSec.load();
                    double smoothed = (prevSpeed > 0) ? prevSpeed * 0.8 + instantSpeed * 0.2 : instantSpeed;
                    batch->speedBytesPerSec = smoothed;
                    if (smoothed > 0 && tb > totalTx) batch->etaSeconds = (double)(tb - totalTx) / smoothed;
                    else batch->etaSeconds = -1.0;
                    lastSpeedBytes = totalTx;
                    lastSpeedTime = now;
                }

                return true;
            };

            // --- Local copy/move (Windows-to-Windows) ---
            if (batch->isLocalCopy) {
                bool ok = false;
                try {
                    // Same-drive move: instant rename (no data copy)
                    bool sameDrive = (item.sourcePath.size() >= 2 && item.destPath.size() >= 2 &&
                                     toupper(item.sourcePath[0]) == toupper(item.destPath[0]));

                    if (batch->isMove && sameDrive && !item.isDirectory) {
                        // Create parent directory if needed
                        auto dp = toFsPath(item.destPath);
                        if (dp.has_parent_path())
                            std::filesystem::create_directories(dp.parent_path());
                        std::filesystem::rename(toFsPath(item.sourcePath), dp);
                        progressCb(item.fileSize, item.fileSize);
                        ok = true;
                    } else if (item.isDirectory) {
                        if (batch->isMove && sameDrive) {
                            std::filesystem::rename(toFsPath(item.sourcePath), toFsPath(item.destPath));
                        } else {
                            std::filesystem::create_directories(toFsPath(item.destPath));
                        }
                        ok = true;
                    } else {
                        // Create parent directory if needed
                        auto dp = toFsPath(item.destPath);
                        if (dp.has_parent_path())
                            std::filesystem::create_directories(dp.parent_path());

                        // Buffered copy with progress reporting
                        FILE* fin = nullptr; FILE* fout = nullptr;
                        _wfopen_s(&fin, toWide(item.sourcePath).c_str(), L"rb");
                        if (!fin) throw std::runtime_error("Cannot open source: " + item.sourcePath);
                        _wfopen_s(&fout, toWide(item.destPath).c_str(), L"wb");
                        if (!fout) { fclose(fin); throw std::runtime_error("Cannot open dest: " + item.destPath); }

                        const size_t bufSize = 4 * 1024 * 1024; // 4MB
                        auto buf = std::make_unique<char[]>(bufSize);
                        uint64_t copied = 0;
                        bool cancelled = false;
                        while (true) {
                            size_t n = fread(buf.get(), 1, bufSize, fin);
                            if (n == 0) break;
                            fwrite(buf.get(), 1, n, fout);
                            copied += n;
                            if (!progressCb(copied, item.fileSize)) { cancelled = true; break; }
                        }
                        fclose(fin);
                        fclose(fout);
                        if (cancelled) {
                            std::filesystem::remove(toFsPath(item.destPath));
                        } else {
                            ok = true;
                            // Cross-drive move: delete source after successful copy
                            if (batch->isMove) {
                                std::filesystem::remove(toFsPath(item.sourcePath));
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    batch->errorMessage = item.displayName + ": " + e.what();
                }

                if (batch->stopRequested.load()) { batch->state = BatchState::Stopped; break; }
                if (!ok) {
                    if (batch->errorMessage.empty())
                        batch->errorMessage = item.displayName + ": " + (batch->isMove ? "move" : "copy") + " failed";
                    batch->state = BatchState::Failed;
                    break;
                }

                bytesCompletedBefore += item.fileSize;
                batch->totalTransferred = bytesCompletedBefore;
                batch->curFileProgress = 1.0f;
                m_lastTransferActivity = std::chrono::steady_clock::now();
                continue; // next file
            }

            // --- Cross-device Android-to-Android transfer ---
            if (batch->isCrossDevice) {
                DeviceClient& srcDev = m_deviceSlots[batch->srcDeviceSlot & 1];
                DeviceClient& dstDev = m_deviceSlots[batch->dstDeviceSlot & 1];
                bool ok = false;

                if (!srcDev.isServerRunning() || !dstDev.isServerRunning()) {
                    batch->state = BatchState::Failed;
                    batch->errorMessage = "Both devices must be connected for cross-device transfer";
                    break;
                }

                if (item.isDirectory) {
                    // Create directory on destination device
                    ok = dstDev.createDirectory(item.destPath);
                } else {
                    // Zero-copy relay: stream directly from device A to device B through PC memory
                    ok = DeviceClient::relayFile(srcDev, dstDev, item.sourcePath, item.destPath,
                                                  item.fileSize, progressCb);
                }

                if (batch->stopRequested.load()) { batch->state = BatchState::Stopped; break; }
                if (!ok) {
                    batch->state = BatchState::Failed;
                    batch->errorMessage = item.displayName + ": cross-device transfer failed";
                    break;
                }

                bytesCompletedBefore += item.fileSize;
                batch->totalTransferred = bytesCompletedBefore;
                batch->curFileProgress = 1.0f;
                m_lastTransferActivity = std::chrono::steady_clock::now();
                continue;
            }

            // Execute transfer via TCP server, with disconnect recovery
            bool ok = false;
            bool isResume = false;
            int maxRetries = 10;

            for (int attempt = 0; attempt <= maxRetries; attempt++) {
                if (batch->stopRequested.load()) break;

                if (attempt > 0) {
                    // Reconnect after disconnect — track disconnected duration
                    auto disconnectStart = std::chrono::steady_clock::now();
                    batch->disconnected = true;
                    batch->errorMessage = "Connection lost - reconnecting... (attempt " +
                        std::to_string(attempt) + "/" + std::to_string(maxRetries) + ")";
                    m_statusMessage = batch->errorMessage;
                    m_statusTime = std::chrono::steady_clock::now();

                    // Wait for device to come back and reconnect (up to 2 minutes)
                    bool reconnected = false;
                    for (int wait = 0; wait < 120 && !m_shutdownTransfer && !batch->stopRequested.load(); wait++) {
                        std::this_thread::sleep_for(std::chrono::seconds(2));

                        batch->errorMessage = "Waiting for device... (" + std::to_string(wait * 2) + "s)";

                        // Poll devices directly from this thread (don't rely on UI thread's m_devices)
                        auto devices = batchDev.getDevices();
                        std::string foundSerial;
                        for (auto& d : devices) {
                            // Match against original serial to avoid reconnecting to a different device
                            if (d.state == "device" && d.serial == originalSerial) { foundSerial = d.serial; break; }
                        }
                        if (foundSerial.empty()) continue;

                        batch->errorMessage = "Device found (" + foundSerial + ") - connecting...";
                        LOG_INFO("Transfer", "Device found: " + foundSerial + " - reconnecting");
                        std::this_thread::sleep_for(std::chrono::seconds(3)); // let device settle

                        // Wait for tethering to finish if poll thread is doing it
                        while (m_tetheringInProgress && !m_shutdownTransfer && !batch->stopRequested.load()) {
                            batch->errorMessage = "Waiting for tethering to complete...";
                            std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        }
                        if (m_shutdownTransfer || batch->stopRequested.load()) break;

                        // Re-enable tethering only if user prefers Direct TCP
                        if (!m_preferAdbForward) {
                            std::string ip = batchDev.detectDeviceIp(foundSerial);
                            if (ip.empty()) {
                                batch->errorMessage = "Re-enabling USB tethering...";
                                LOG_INFO("Transfer", "No tethering IP - re-enabling tethering");
                                m_tetheringInProgress = true;
                                batchDev.tryEnableUsbTethering(foundSerial);
                                m_tetheringInProgress = false;
                            }
                        }

                        if (batchDev.startServer(foundSerial, m_preferAdbForward)) {
                            std::string connMode = batchDev.isDirectConnection()
                                ? "Direct TCP: " + batchDev.deviceIp() : "ADB Forward";
                            m_statusMessage = "Reconnected via " + connMode + " - resuming transfer";
                            m_statusTime = std::chrono::steady_clock::now();

                            // Update m_lastDeviceSerial so poll thread doesn't
                            // treat this as a "new device" and re-run tethering
                            m_lastDeviceSerial = foundSerial;

                            batch->disconnected = false;
                            batch->errorMessage.clear();
                            isResume = true;
                            reconnected = true;
                            // Accumulate disconnected time
                            batch->pausedSeconds = batch->pausedSeconds.load() +
                                std::chrono::duration<double>(std::chrono::steady_clock::now() - disconnectStart).count();
                            // Reset speed tracking
                            lastSpeedBytes = batch->totalTransferred.load();
                            lastSpeedTime = std::chrono::steady_clock::now();
                            break;
                        } else {
                            batch->errorMessage = "Server start failed: " + batchDev.lastError();
                        }
                    }
                    if (!reconnected) continue; // retry loop
                }

                if (item.isDirectory) {
                    // Create directory on destination
                    if (batch->isPull) {
                        try { std::filesystem::create_directories(item.destPath); } catch (...) {}
                    } else {
                        batchDev.createDirectory(item.destPath);
                    }
                    ok = true;
                    break; // no retry needed for mkdir
                } else if (item.isMcrawVirtual) {
                    // MCRAW virtual item extraction
                    if (batch->isPull) {
                        // Android MCRAW → Windows: use pullMcrawItem
                        uint64_t outSize = 0;
                        ok = batchDev.pullMcrawItem(item.mcrawPath, item.virtualName,
                                                     item.destPath, outSize, progressCb);
                        if (ok && outSize > 0) item.fileSize = outSize;
                    } else {
                        // Windows MCRAW → Android: extract locally to temp, then push
                        std::string tmpPath = item.destPath + ".tmp";
                        ok = extractLocalMcrawItem(item.mcrawPath, item.virtualName, tmpPath);
                        if (ok) {
                            uint64_t tmpSize = (uint64_t)std::filesystem::file_size(toFsPath(tmpPath));
                            item.fileSize = tmpSize;
                            ok = batchDev.pushFile(tmpPath, item.destPath, tmpSize, progressCb);
                            std::filesystem::remove(toFsPath(tmpPath));
                        }
                    }
                } else if (batch->isPull) {
                    LOG_INFO("Transfer", "Batch pull: " + item.sourcePath + " -> " + item.destPath);
                    uint64_t outSize = 0;
                    if (isResume) {
                        ok = batchDev.resumePullFile(item.sourcePath, item.destPath, outSize, progressCb);
                    } else {
                        ok = batchDev.pullFile(item.sourcePath, item.destPath, outSize, progressCb);
                    }
                    LOG_INFO("Transfer", "Batch pull result: " + std::string(ok ? "OK" : "FAIL") +
                             " error=" + batchDev.lastError());
                    if (ok && outSize > 0) item.fileSize = outSize;
                } else {
                    if (isResume) {
                        ok = batchDev.resumePushFile(item.sourcePath, item.destPath, item.fileSize, progressCb);
                    } else {
                        ok = batchDev.pushFile(item.sourcePath, item.destPath, item.fileSize, progressCb);
                    }
                }

                if (ok) break; // success

                // Check if it was a user cancel vs connection error
                if (batch->stopRequested.load()) break;
                if (batchDev.lastError() == "Cancelled") break;

                // Local file errors (permission denied, read-only, etc.) — don't retry
                {
                    std::string err = batchDev.lastError();
                    if (err.find("Failed to create local file") != std::string::npos ||
                        err.find("Cannot open") != std::string::npos ||
                        err.find("Access is denied") != std::string::npos) {
                        batch->state = BatchState::Failed;
                        batch->errorMessage = err + "\n\nThe destination may require administrator rights or be read-only.";
                        break;
                    }
                }

                // If we've used all automatic retries, ask the user
                if (attempt == maxRetries) {
                    batch->errorMessage = "Connection failed after " + std::to_string(maxRetries) +
                        " retries. Click Retry or Stop.";
                    batch->waitingForUserRetry = true;
                    batch->userRetryRequested = false;

                    // Wait for user to click Retry or Stop (uses CV instead of busy-wait)
                    {
                        std::unique_lock<std::mutex> lk(m_batchMutex);
                        m_batchCV.wait(lk, [&]() {
                            return m_shutdownTransfer || batch->stopRequested.load() || batch->userRetryRequested.load();
                        });
                    }

                    batch->waitingForUserRetry = false;
                    if (batch->userRetryRequested.load()) {
                        batch->userRetryRequested = false;
                        batch->errorMessage.clear();
                        attempt = -1; // reset retry counter (will become 0 on loop increment)
                        isResume = true;
                        continue;
                    }
                    break; // user chose stop
                }

                // Connection error - will retry
                isResume = true;
            }

            if (batch->stopRequested.load()) { batch->state = BatchState::Stopped; break; }

            if (!ok) {
                batch->state = BatchState::Failed;
                batch->errorMessage = item.displayName + ": " + batchDev.lastError();
                break;
            }

            m_lastTransferActivity = std::chrono::steady_clock::now();

            // CRC32 verification with retry (can be disabled in preferences)
            // Skip CRC for MCRAW virtual items — data is generated on-the-fly, no remote file to verify against
            if (!item.isDirectory && !item.isMcrawVirtual && !batch->isLocalCopy && !batch->stopRequested.load() &&
                batchDev.isServerRunning() && m_prefs.enableCrcVerification) {
                std::string crcDetail;
                std::string remotePath = batch->isPull ? item.sourcePath : item.destPath;
                std::string localPath  = batch->isPull ? item.destPath   : item.sourcePath;
                batch->crcFileName = item.displayName;
                batch->crcProgress = 0.0f;
                batch->crcPhase = 0;
                batch->state = BatchState::Verifying;
                batch->errorMessage = "Verifying: " + item.displayName;

                bool crcOk = false;
                double crcRemoteMs = 0, crcLocalMs = 0;
                auto crcTotalStart = std::chrono::steady_clock::now();
                for (int crcAttempt = 0; crcAttempt < 3; crcAttempt++) {
                    batch->crcProgress = 0.0f;
                    batch->crcPhase = 0;
                    if (batchDev.verifyFileCrc(remotePath, localPath, crcDetail, &batch->crcProgress, &batch->crcPhase,
                                               &crcRemoteMs, &crcLocalMs)) {
                        crcOk = true;
                        break;
                    }
                    // If it's a connection error (not actual mismatch), retry after reconnect
                    if (crcDetail.find("Local:") != std::string::npos) break; // actual CRC mismatch, don't retry
                    LOG_WARN("Transfer", "CRC check failed (attempt " + std::to_string(crcAttempt+1) + "/3): " + crcDetail);
                    if (crcAttempt < 2 && !batchDev.isServerRunning()) {
                        // Try to reconnect
                        batch->errorMessage = "CRC check failed - reconnecting...";
                        std::this_thread::sleep_for(std::chrono::seconds(2));
                        batchDev.startServer(originalSerial);
                        if (!batchDev.isServerRunning()) break;
                        batch->errorMessage = "Retrying CRC verification: " + item.displayName;
                    }
                }
                batch->state = BatchState::Running;
                batch->crcFileName.clear();
                batch->crcProgress = 0.0f;
                batch->crcPhase = 0;

                double crcTotalMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - crcTotalStart).count();

                TransferBatch::CrcResult crcRes;
                crcRes.fileName = item.displayName;
                crcRes.passed = crcOk;
                crcRes.detail = crcDetail;
                crcRes.remoteMs = crcRemoteMs;
                crcRes.localMs = crcLocalMs;
                crcRes.totalMs = crcTotalMs;
                crcRes.fileSize = item.fileSize;
                crcRes.sourcePath = item.sourcePath;  // original batch paths (not CRC-rearranged)
                crcRes.destPath = item.destPath;
                batch->crcResults.push_back(std::move(crcRes));

                if (!crcOk) {
                    LOG_WARN("Transfer", "CRC failed for " + item.displayName + ": " + crcDetail + " — continuing batch");
                    batch->errorMessage = "CRC failed: " + item.displayName + " — continuing...";
                } else {
                    batch->errorMessage.clear();
                }
            }

            bytesCompletedBefore += item.fileSize;
            batch->totalTransferred = bytesCompletedBefore;
            batch->curFileProgress = 1.0f;
        }

        // Clean up partial files and flush stale data on stop/cancel/error
        if (batch->state.load() == BatchState::Stopped || batch->state.load() == BatchState::Failed) {
            // Delete the partially-transferred file (the one that was in progress when stopped)
            int curIdx = batch->currentFileIndex.load();
            if (curIdx >= 0 && curIdx < batch->totalFiles()) {
                auto& item = batch->files[curIdx];
                if (!item.isDirectory && batch->isPull) {
                    // Pull: partial file is on Windows
                    try {
                        if (std::filesystem::exists(toFsPath(item.destPath))) {
                            std::filesystem::remove(toFsPath(item.destPath));
                            LOG_INFO("Transfer", "Deleted partial file: " + item.destPath);
                        }
                    } catch (...) {}
                } else if (!item.isDirectory && !batch->isPull && !batch->isLocalCopy) {
                    // Push: partial file is on Android
                    batchDev.deleteFile(item.destPath);
                    LOG_INFO("Transfer", "Deleted partial remote file: " + item.destPath);
                } else if (!item.isDirectory && batch->isLocalCopy) {
                    // Local copy: partial file is on Windows
                    try {
                        if (std::filesystem::exists(toFsPath(item.destPath))) {
                            std::filesystem::remove(toFsPath(item.destPath));
                            LOG_INFO("Transfer", "Deleted partial file: " + item.destPath);
                        }
                    } catch (...) {}
                }
            }
            batchDev.flushStaleData();
            batchDev.restoreTimeouts();
        }

        // Finalize batch
        if (batch->state.load() == BatchState::Running || batch->state.load() == BatchState::Verifying) {
            batch->totalProgress = 1.0f;
            batch->totalTransferred = batch->totalBytes.load();
            auto totalTime = std::chrono::steady_clock::now() - batch->startTime;
            double wallSec = std::chrono::duration<double>(totalTime).count();
            double activeSec = wallSec - batch->pausedSeconds.load(); // exclude paused/disconnected time
            if (activeSec < 0.1) activeSec = 0.1;
            double avgSpeed = (activeSec > 0) ? (double)batch->totalTransferred.load() / activeSec : 0;
            batch->finalAvgSpeed = avgSpeed;
            batch->finalTimeSec = activeSec;
            batch->speedBytesPerSec = 0;
            batch->etaSeconds = -1;
            batch->state = BatchState::Completed;

            std::string skippedStr = (batch->skippedFiles > 0)
                ? " (" + std::to_string(batch->skippedFiles) + " skipped)" : "";
            m_statusMessage = "Batch done: " + std::to_string(batch->totalFiles()) + " files" + skippedStr + ", " +
                formatSize(batch->totalTransferred.load()) + " @ " + formatSpeed(avgSpeed);
            m_statusTime = std::chrono::steady_clock::now();

            m_leftPanel.needsRefresh = true;
            m_rightPanel.needsRefresh = true;
        }
    }
}

std::string App::queryDeviceDisplayName(const std::string& serial) {
    // Try properties in priority order: marketing name, then brand + model fallback
    const char* props[] = {
        "ro.product.marketname",
        "ro.product.vendor.marketname",
        "ro.config.marketing_name",
    };
    for (auto prop : props) {
        std::string val = m_device.runAdbCommand("-s " + serial + " shell getprop " + prop);
        while (!val.empty() && (val.back() == '\r' || val.back() == '\n')) val.pop_back();
        if (!val.empty() && val.find("error") == std::string::npos) return val;
    }
    // Fallback: "Brand Model" e.g. "Samsung SM-S911B"
    std::string brand = m_device.runAdbCommand("-s " + serial + " shell getprop ro.product.brand");
    std::string model = m_device.runAdbCommand("-s " + serial + " shell getprop ro.product.model");
    while (!brand.empty() && (brand.back() == '\r' || brand.back() == '\n')) brand.pop_back();
    while (!model.empty() && (model.back() == '\r' || model.back() == '\n')) model.pop_back();
    if (!brand.empty() && brand.find("error") == std::string::npos) {
        // Capitalize first letter
        if (!brand.empty()) brand[0] = (char)toupper((unsigned char)brand[0]);
        if (!model.empty() && model.find("error") == std::string::npos)
            return brand + " " + model;
        return brand;
    }
    if (!model.empty() && model.find("error") == std::string::npos) return model;
    return "";
}

std::string App::formatSize(uint64_t bytes) {
    if (bytes == 0) return "0 B";
    const char* u[] = {"B","KB","MB","GB","TB"};
    int i = 0; double s = (double)bytes;
    while (s >= 1024.0 && i < 4) { s /= 1024.0; i++; }
    char buf[32];
    if (i == 0) snprintf(buf, sizeof(buf), "%llu B", bytes);
    else snprintf(buf, sizeof(buf), "%.1f %s", s, u[i]);
    return buf;
}

std::string App::formatSpeed(double bps) {
    if (bps <= 0) return "0 B/s";
    const char* u[] = {"B/s","KB/s","MB/s","GB/s"};
    int i = 0; double v = bps;
    while (v >= 1024.0 && i < 3) { v /= 1024.0; i++; }
    char buf[32]; snprintf(buf, sizeof(buf), "%.1f %s", v, u[i]);
    return buf;
}

std::string App::formatETA(double sec) {
    if (sec < 0) return "";
    if (sec < 60) { char b[16]; snprintf(b,sizeof(b),"%.0fs",sec); return b; }
    if (sec < 3600) { int m=(int)(sec/60),s=(int)sec%60; char b[16]; snprintf(b,sizeof(b),"%dm %02ds",m,s); return b; }
    int h=(int)(sec/3600),m=((int)sec%3600)/60; char b[16]; snprintf(b,sizeof(b),"%dh %02dm",h,m); return b;
}

std::string App::getFileIcon(const std::string& name, bool isDir) {
    if (isDir) return "[D]";
    auto p = name.rfind('.'); if (p == std::string::npos) return "   ";
    std::string ext = name.substr(p);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext==".jpg"||ext==".jpeg"||ext==".png"||ext==".gif"||ext==".bmp"||ext==".webp"||ext==".dng") return "[I]";
    if (ext==".mp4"||ext==".mkv"||ext==".avi"||ext==".mov"||ext==".webm") return "[V]";
    if (ext==".mp3"||ext==".wav"||ext==".ogg"||ext==".flac"||ext==".aac") return "[A]";
    if (ext==".apk"||ext==".xapk") return "[K]";
    if (ext==".zip"||ext==".rar"||ext==".7z"||ext==".tar"||ext==".gz") return "[Z]";
    if (ext==".mcraw") return "[M]";
    if (ext==".pdf") return "[P]";
    if (ext==".txt"||ext==".log"||ext==".md"||ext==".json"||ext==".xml"||ext==".csv") return "[T]";
    return "   ";
}

std::vector<std::string> App::getWindowsDrives() {
    std::vector<std::string> drives;
    DWORD mask = GetLogicalDrives();
    for (char c='A'; c<='Z'; c++) { if (mask & 1) drives.push_back(std::string(1,c)+":"); mask >>= 1; }
    return drives;
}
