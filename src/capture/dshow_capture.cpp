#include "../common/input_limits.h"
#include <atomic>
#include "dshow_capture.h"

#include <dshow.h>
#include <wrl/client.h>
#include <vector>
#include <chrono>
#include <mutex>
#include <string>
#include <sstream>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <limits>

using Microsoft::WRL::ComPtr;

namespace NitLink {

static void DLog(const std::wstring& m) {
    OutputDebugStringW((L"[NitLink/DShow] " + m + L"\n").c_str());
}

// ---- AM_MEDIA_TYPE helpers (the DirectShow base classes are not linked) ----
static void FreeMTContents(AM_MEDIA_TYPE* mt) {
    if (!mt) return;
    if (mt->cbFormat && mt->pbFormat) { CoTaskMemFree(mt->pbFormat); }
    mt->cbFormat = 0; mt->pbFormat = nullptr;
    if (mt->pUnk) { mt->pUnk->Release(); mt->pUnk = nullptr; }
}
static void DeleteMT(AM_MEDIA_TYPE* mt) {
    if (!mt) return;
    FreeMTContents(mt);
    CoTaskMemFree(mt);
}
static HRESULT CopyMT(AM_MEDIA_TYPE* dst, const AM_MEDIA_TYPE* src) {
    if (!dst || !src) return E_POINTER;
    *dst = {};
    if (src->cbFormat > 64 * 1024 || (src->cbFormat && !src->pbFormat)) return E_INVALIDARG;
    AM_MEDIA_TYPE copy = *src;
    copy.pbFormat = nullptr;
    if (src->cbFormat) {
        copy.pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(src->cbFormat));
        if (!copy.pbFormat) return E_OUTOFMEMORY;
        memcpy(copy.pbFormat, src->pbFormat, src->cbFormat);
    }
    if (copy.pUnk) copy.pUnk->AddRef();
    *dst = copy;
    return S_OK;
}
static bool ValidNV12Type(const AM_MEDIA_TYPE* mt) {
    if (!mt || mt->majortype != MEDIATYPE_Video || mt->subtype != MFVideoFormat_NV12 ||
        mt->formattype != FORMAT_VideoInfo || !mt->pbFormat ||
        mt->cbFormat < sizeof(VIDEOINFOHEADER) || mt->cbFormat > 64 * 1024) return false;
    const auto& header = *reinterpret_cast<const VIDEOINFOHEADER*>(mt->pbFormat);
    const int64_t height = header.bmiHeader.biHeight;
    const uint64_t absHeight = height < 0 ? -height : height;
    uint32_t row = 0, bytes = 0;
    return header.bmiHeader.biWidth > 0 && absHeight <= kMaxVideoDimension &&
        header.AvgTimePerFrame >= 10000 &&
        FrameLayout(static_cast<uint32_t>(header.bmiHeader.biWidth), static_cast<uint32_t>(absHeight),
                    PixelLayout::Nv12, row, bytes) &&
        FrameCapacity(static_cast<uint32_t>(header.bmiHeader.biWidth), static_cast<uint32_t>(absHeight), row);
}
static AM_MEDIA_TYPE* CreateMT(const AM_MEDIA_TYPE* src) {
    auto* mt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
    if (!mt) return nullptr;
    ZeroMemory(mt, sizeof(*mt));
    if (FAILED(CopyMT(mt, src))) { CoTaskMemFree(mt); return nullptr; }
    return mt;
}

namespace {

class SinkFilter; // fwd

// ---------------------------------------------------------------------------
// IEnumMediaTypes over a single (optional) offered type.
// ---------------------------------------------------------------------------
class SinkEnumMediaTypes : public IEnumMediaTypes {
public:
    SinkEnumMediaTypes(const AM_MEDIA_TYPE* mt) {
        if (mt) m_has = SUCCEEDED(CopyMT(&m_mt, mt));
    }
    ~SinkEnumMediaTypes() { if (m_has) FreeMTContents(&m_mt); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes) { *ppv = static_cast<IEnumMediaTypes*>(this); AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() { long c = InterlockedDecrement(&m_ref); if (!c) delete this; return c; }

    STDMETHODIMP Next(ULONG cMT, AM_MEDIA_TYPE** ppMT, ULONG* pcFetched) {
        if (!ppMT) return E_POINTER;
        ULONG got = 0;
        while (got < cMT && m_has && m_pos < 1) {
            ppMT[got] = CreateMT(&m_mt);
            if (!ppMT[got]) break;
            got++; m_pos++;
        }
        if (pcFetched) *pcFetched = got;
        return (got == cMT) ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG c) { m_pos += c; return (m_pos <= 1) ? S_OK : S_FALSE; }
    STDMETHODIMP Reset() { m_pos = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumMediaTypes** ppEnum) {
        if (!ppEnum) return E_POINTER;
        *ppEnum = new SinkEnumMediaTypes(m_has ? &m_mt : nullptr);
        return S_OK;
    }
private:
    volatile long m_ref = 1;
    AM_MEDIA_TYPE m_mt{};
    bool m_has = false;
    ULONG m_pos = 0;
};

// ---------------------------------------------------------------------------
// The sink filter's single input pin: IPin + IMemInputPin. Refcount delegates
// to the owning filter so external references keep the filter alive.
// ---------------------------------------------------------------------------
class SinkPin : public IPin, public IMemInputPin {
public:
    explicit SinkPin(SinkFilter* f) : m_filter(f) {}
    ~SinkPin() { FreeMTContents(&m_offerMt); FreeMTContents(&m_connMt); }

    void SetOfferType(const AM_MEDIA_TYPE* mt) {
        FreeMTContents(&m_offerMt);
        m_hasOffer = mt && SUCCEEDED(CopyMT(&m_offerMt, mt));
    }

    // IUnknown (delegates lifetime to the filter)
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv);
    STDMETHODIMP_(ULONG) AddRef();
    STDMETHODIMP_(ULONG) Release();

    // IPin
    STDMETHODIMP Connect(IPin*, const AM_MEDIA_TYPE*) { return E_UNEXPECTED; } // input pin
    STDMETHODIMP ReceiveConnection(IPin* c, const AM_MEDIA_TYPE* pmt);
    STDMETHODIMP Disconnect();
    STDMETHODIMP ConnectedTo(IPin** p);
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt);
    STDMETHODIMP QueryPinInfo(PIN_INFO* pInfo);
    STDMETHODIMP QueryDirection(PIN_DIRECTION* d) { if (!d) return E_POINTER; *d = PINDIR_INPUT; return S_OK; }
    STDMETHODIMP QueryId(LPWSTR* Id);
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt);
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** e);
    STDMETHODIMP QueryInternalConnections(IPin**, ULONG*) { return E_NOTIMPL; }
    STDMETHODIMP EndOfStream() { return S_OK; }
    STDMETHODIMP BeginFlush() { m_flushing = true; return S_OK; }
    STDMETHODIMP EndFlush() { m_flushing = false; return S_OK; }
    STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) { return S_OK; }

    // IMemInputPin
    STDMETHODIMP GetAllocator(IMemAllocator** a) { (void)a; return VFW_E_NO_ALLOCATOR; }
    STDMETHODIMP NotifyAllocator(IMemAllocator*, BOOL) { return S_OK; }
    STDMETHODIMP GetAllocatorRequirements(ALLOCATOR_PROPERTIES*) { return E_NOTIMPL; }
    STDMETHODIMP Receive(IMediaSample* s);
    STDMETHODIMP ReceiveMultiple(IMediaSample** s, long n, long* nProc);
    STDMETHODIMP ReceiveCanBlock() { return S_FALSE; }

    SinkFilter*   m_filter = nullptr;
    ComPtr<IPin>  m_connected;
    AM_MEDIA_TYPE m_offerMt{};  bool m_hasOffer = false;
    AM_MEDIA_TYPE m_connMt{};   bool m_hasConn  = false;
    std::atomic<bool> m_flushing{false};
};

// ---------------------------------------------------------------------------
// IEnumPins over the filter's single pin.
// ---------------------------------------------------------------------------
class SinkEnumPins : public IEnumPins {
public:
    explicit SinkEnumPins(IPin* p) : m_pin(p) { if (m_pin) m_pin->AddRef(); }
    ~SinkEnumPins() { if (m_pin) m_pin->Release(); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumPins) { *ppv = static_cast<IEnumPins*>(this); AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() { long c = InterlockedDecrement(&m_ref); if (!c) delete this; return c; }

    STDMETHODIMP Next(ULONG cPins, IPin** ppPins, ULONG* pcFetched) {
        if (!ppPins) return E_POINTER;
        ULONG got = 0;
        if (m_pos < 1 && cPins >= 1 && m_pin) { m_pin->AddRef(); ppPins[0] = m_pin; got = 1; m_pos = 1; }
        if (pcFetched) *pcFetched = got;
        return (got == cPins) ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG c) { m_pos += c; return (m_pos <= 1) ? S_OK : S_FALSE; }
    STDMETHODIMP Reset() { m_pos = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumPins** ppEnum) { if (!ppEnum) return E_POINTER; *ppEnum = new SinkEnumPins(m_pin); return S_OK; }
private:
    volatile long m_ref = 1;
    IPin* m_pin = nullptr;
    ULONG m_pos = 0;
};

// ---------------------------------------------------------------------------
// The sink filter: IBaseFilter + IAMFilterMiscFlags (renderer flag). Owns the
// single input pin. Forwards each received sample to the owning DShowGraph.
// ---------------------------------------------------------------------------
class SinkFilter : public IBaseFilter, public IAMFilterMiscFlags {
public:
    explicit SinkFilter(DShowGraph* owner) : m_owner(owner), m_pin(this) {}

    // IUnknown (single override serves both interface vtables)
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IPersist ||
            riid == IID_IMediaFilter || riid == IID_IBaseFilter) {
            *ppv = static_cast<IBaseFilter*>(this);
        } else if (riid == IID_IAMFilterMiscFlags) {
            *ppv = static_cast<IAMFilterMiscFlags*>(this);
        } else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() { long c = InterlockedDecrement(&m_ref); if (!c) delete this; return c; }

    // IPersist
    STDMETHODIMP GetClassID(CLSID* p) { if (!p) return E_POINTER; *p = GUID_NULL; return S_OK; }

    // IMediaFilter
    STDMETHODIMP Stop()  { m_state = State_Stopped; return S_OK; }
    STDMETHODIMP Pause() { m_state = State_Paused;  return S_OK; }
    STDMETHODIMP Run(REFERENCE_TIME) { m_state = State_Running; return S_OK; }
    STDMETHODIMP GetState(DWORD, FILTER_STATE* s) { if (!s) return E_POINTER; *s = m_state; return S_OK; }
    STDMETHODIMP SetSyncSource(IReferenceClock* c) { m_clock = c; return S_OK; }
    STDMETHODIMP GetSyncSource(IReferenceClock** c) { if (!c) return E_POINTER; *c = m_clock.Get(); if (*c) (*c)->AddRef(); return S_OK; }

    // IBaseFilter
    STDMETHODIMP EnumPins(IEnumPins** e) { if (!e) return E_POINTER; *e = new SinkEnumPins(static_cast<IPin*>(&m_pin)); return S_OK; }
    STDMETHODIMP FindPin(LPCWSTR Id, IPin** p) {
        if (!p) return E_POINTER;
        if (Id && wcscmp(Id, L"In") == 0) { *p = static_cast<IPin*>(&m_pin); (*p)->AddRef(); return S_OK; }
        *p = nullptr; return VFW_E_NOT_FOUND;
    }
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* pInfo) {
        if (!pInfo) return E_POINTER;
        ZeroMemory(pInfo, sizeof(*pInfo));
        wcscpy_s(pInfo->achName, L"NitLinkSink");
        pInfo->pGraph = m_graph;
        if (m_graph) m_graph->AddRef();
        return S_OK;
    }
    STDMETHODIMP JoinFilterGraph(IFilterGraph* g, LPCWSTR) { m_graph = g; return S_OK; } // weak ref per DShow rules
    STDMETHODIMP QueryVendorInfo(LPWSTR*) { return E_NOTIMPL; }

    // IAMFilterMiscFlags
    STDMETHODIMP_(ULONG) GetMiscFlags() { return AM_FILTER_MISC_FLAGS_IS_RENDERER; }

    DShowGraph*       m_owner = nullptr;
    SinkPin           m_pin;
    volatile long     m_ref = 1;
    FILTER_STATE      m_state = State_Stopped;
    IFilterGraph*     m_graph = nullptr;          // weak (no AddRef: avoids cycle)
    ComPtr<IReferenceClock> m_clock;
};

// ---- SinkPin IUnknown delegates to the filter ----
STDMETHODIMP SinkPin::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IPin) { *ppv = static_cast<IPin*>(this); }
    else if (riid == IID_IMemInputPin) { *ppv = static_cast<IMemInputPin*>(this); }
    else { *ppv = nullptr; return E_NOINTERFACE; }
    AddRef();
    return S_OK;
}
STDMETHODIMP_(ULONG) SinkPin::AddRef() { return m_filter->AddRef(); }
STDMETHODIMP_(ULONG) SinkPin::Release() { return m_filter->Release(); }

STDMETHODIMP SinkPin::QueryAccept(const AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    if (ValidNV12Type(pmt)) return S_OK;
    return S_FALSE;
}
STDMETHODIMP SinkPin::ReceiveConnection(IPin* c, const AM_MEDIA_TYPE* pmt) {
    if (!c || !pmt) return E_POINTER;
    if (m_connected) return VFW_E_ALREADY_CONNECTED;
    if (QueryAccept(pmt) != S_OK) return VFW_E_TYPE_NOT_ACCEPTED;
    FreeMTContents(&m_connMt);
    m_hasConn = false;
    const HRESULT hr = CopyMT(&m_connMt, pmt);
    if (FAILED(hr)) return hr;
    m_hasConn = true;
    m_connected = c;
    return S_OK;
}
STDMETHODIMP SinkPin::Disconnect() {
    if (!m_connected) return S_FALSE;
    m_connected.Reset();
    FreeMTContents(&m_connMt); m_hasConn = false;
    return S_OK;
}
STDMETHODIMP SinkPin::ConnectedTo(IPin** p) {
    if (!p) return E_POINTER;
    if (!m_connected) { *p = nullptr; return VFW_E_NOT_CONNECTED; }
    *p = m_connected.Get(); (*p)->AddRef(); return S_OK;
}
STDMETHODIMP SinkPin::ConnectionMediaType(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    if (!m_hasConn) { ZeroMemory(pmt, sizeof(*pmt)); return VFW_E_NOT_CONNECTED; }
    return CopyMT(pmt, &m_connMt);
}
STDMETHODIMP SinkPin::QueryPinInfo(PIN_INFO* pInfo) {
    if (!pInfo) return E_POINTER;
    pInfo->pFilter = static_cast<IBaseFilter*>(m_filter);
    if (m_filter) m_filter->AddRef();
    pInfo->dir = PINDIR_INPUT;
    wcscpy_s(pInfo->achName, L"In");
    return S_OK;
}
STDMETHODIMP SinkPin::QueryId(LPWSTR* Id) {
    if (!Id) return E_POINTER;
    const wchar_t* s = L"In";
    size_t n = (wcslen(s) + 1) * sizeof(wchar_t);
    *Id = (LPWSTR)CoTaskMemAlloc(n);
    if (!*Id) return E_OUTOFMEMORY;
    memcpy(*Id, s, n);
    return S_OK;
}
STDMETHODIMP SinkPin::EnumMediaTypes(IEnumMediaTypes** e) {
    if (!e) return E_POINTER;
    *e = new SinkEnumMediaTypes(m_hasOffer ? &m_offerMt : nullptr);
    return S_OK;
}
STDMETHODIMP SinkPin::ReceiveMultiple(IMediaSample** s, long n, long* nProc) {
    if (nProc) *nProc = 0;
    if (n < 0 || (n && !s)) return E_INVALIDARG;
    long done = 0;
    for (long i = 0; i < n; i++) { if (Receive(s[i]) != S_OK) break; done++; }
    if (nProc) *nProc = done;
    return (done == n) ? S_OK : S_FALSE;
}

// ---- pin/filter helpers ----
static ComPtr<IPin> GetPinByDir(IBaseFilter* f, PIN_DIRECTION want) {
    ComPtr<IEnumPins> en;
    if (FAILED(f->EnumPins(&en)) || !en) return nullptr;
    ComPtr<IPin> pin;
    while (en->Next(1, &pin, nullptr) == S_OK) {
        PIN_DIRECTION d;
        if (SUCCEEDED(pin->QueryDirection(&d)) && d == want) return pin;
        pin.Reset();
    }
    return nullptr;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// The graph wrapper (PIMPL target).
// ---------------------------------------------------------------------------
struct DShowGraph {
    ComPtr<IGraphBuilder> graph;
    ComPtr<IMediaControl> control;
    ComPtr<IBaseFilter>   capture;
    ComPtr<IBaseFilter>   sinkBase;   // owns the SinkFilter
    SinkFilter*           sink = nullptr;

    FrameCallback         cb;
    std::mutex            cbMutex;
    bool                  firstSampleLogged = false;

    void OnSample(IMediaSample* s) {
        if (!s) return;
        BYTE* p = nullptr;
        if (FAILED(s->GetPointer(&p)) || !p) return;
        long len = s->GetActualDataLength();
        if (len <= 0 || len > s->GetSize() || static_cast<uint64_t>(len) > kMaxFrameBytes) return;

        REFERENCE_TIME t0 = 0, t1 = 0; int64_t ts = 0;
        if (s->GetTime(&t0, &t1) == S_OK) ts = (int64_t)t0;  // 100ns units

        const int64_t wall = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        if (!firstSampleLogged) {
            firstSampleLogged = true;
            DLog(L"First sample via custom filter: " + std::to_wstring(len) + L" bytes");
        }

        FrameCallback local;
        { std::lock_guard<std::mutex> lk(cbMutex); local = cb; }
        // DeviceTimestamp is a Media Foundation concept; not available here, pass 0.
        if (local) local(p, (uint32_t)len, ts, wall, 0);
    }
};

// SinkPin::Receive needs the complete DShowGraph definition.
namespace {
STDMETHODIMP SinkPin::Receive(IMediaSample* s) {
    if (m_flushing) return S_FALSE;
    if (s && m_filter && m_filter->m_owner) m_filter->m_owner->OnSample(s);
    return S_OK;
}
} // anonymous namespace

// ---------------------------------------------------------------------------
// DShowCapture
// ---------------------------------------------------------------------------
DShowCapture::DShowCapture() = default;
DShowCapture::~DShowCapture() { Close(); }

void DShowCapture::ReadFrameRateFromInterval(int64_t interval)
{
    // AvgTimePerFrame is the negotiated period in 100 ns units. Populate the
    // ratio too, so telemetry cannot reuse CaptureFormat's default 60/1.
    const bool known = interval > 0 &&
        static_cast<uint64_t>(interval) <= std::numeric_limits<uint32_t>::max();
    m_format.fpsNumerator = known ? 10000000u : 0;
    m_format.fpsDenominator = known ? static_cast<uint32_t>(interval) : 1;
    m_format.fps = m_format.fpsNumerator / m_format.fpsDenominator;
}

static ComPtr<IBaseFilter> FindCaptureFilter(const std::wstring& name) {
    ComPtr<ICreateDevEnum> sysEnum;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(sysEnum.GetAddressOf()))) || !sysEnum) return nullptr;
    ComPtr<IEnumMoniker> en;
    HRESULT hr = sysEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &en, 0);
    if (hr != S_OK || !en) return nullptr;

    ComPtr<IMoniker> mon;
    while (en->Next(1, &mon, nullptr) == S_OK) {
        ComPtr<IPropertyBag> bag;
        std::wstring friendly;
        if (SUCCEEDED(mon->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(bag.GetAddressOf()))) && bag) {
            VARIANT v; VariantInit(&v);
            if (SUCCEEDED(bag->Read(L"FriendlyName", &v, nullptr)) && v.vt == VT_BSTR && v.bstrVal) {
                friendly = v.bstrVal;
            }
            VariantClear(&v);
        }
        if (friendly == name) {
            ComPtr<IBaseFilter> f;
            if (SUCCEEDED(mon->BindToObject(nullptr, nullptr, IID_PPV_ARGS(f.GetAddressOf()))) && f) return f;
        }
        mon.Reset();
    }
    return nullptr;
}

// Set NV12 1920x1080 at the highest available fps on the capture pin and return
// the negotiated media type (caller frees with DeleteMT).
static AM_MEDIA_TYPE* ConfigureNV12(IPin* outPin) {
    ComPtr<IAMStreamConfig> cfg;
    if (FAILED(outPin->QueryInterface(IID_PPV_ARGS(cfg.GetAddressOf()))) || !cfg) return nullptr;

    int count = 0, size = 0;
    if (FAILED(cfg->GetNumberOfCapabilities(&count, &size)) || count <= 0) return nullptr;
    if (count > 4096 || size > 64 * 1024 || size < (int)sizeof(VIDEO_STREAM_CONFIG_CAPS)) return nullptr;

    std::vector<BYTE> capsbuf(size);
    AM_MEDIA_TYPE* best = nullptr;

    for (int i = 0; i < count; i++) {
        AM_MEDIA_TYPE* mt = nullptr;
        if (cfg->GetStreamCaps(i, &mt, capsbuf.data()) != S_OK || !mt) continue;

        bool keep = false;
        if (mt->majortype == MEDIATYPE_Video && mt->subtype == MFVideoFormat_NV12 &&
            mt->formattype == FORMAT_VideoInfo && mt->pbFormat &&
            mt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
            VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)mt->pbFormat;
            if (vih->bmiHeader.biWidth == 1920 && std::abs(static_cast<int64_t>(vih->bmiHeader.biHeight)) == 1080) {
                auto* vcaps = (VIDEO_STREAM_CONFIG_CAPS*)capsbuf.data();
                REFERENCE_TIME want = 69444; // ~144 fps
                if (want < vcaps->MinFrameInterval) want = vcaps->MinFrameInterval;
                vih->AvgTimePerFrame = want;
                if (best) DeleteMT(best);
                best = mt;
                keep = true;
            }
        }
        if (!keep) DeleteMT(mt);
    }

    if (!best) return nullptr;

    if (FAILED(cfg->SetFormat(best))) { DeleteMT(best); return nullptr; }

    AM_MEDIA_TYPE* cur = nullptr;
    const HRESULT readback = cfg->GetFormat(&cur);
    DeleteMT(best);
    if (readback == S_OK && ValidNV12Type(cur)) return cur;
    DeleteMT(cur);
    return nullptr;
}

bool DShowCapture::Open(const DeviceInfo& device) {
    m_deviceName = device.name;
    m_impl = std::make_unique<DShowGraph>();

    if (FAILED(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(m_impl->graph.GetAddressOf()))) || !m_impl->graph) {
        DLog(L"CoCreateInstance(FilterGraph) failed"); return false;
    }
    if (FAILED(m_impl->graph->QueryInterface(IID_PPV_ARGS(m_impl->control.GetAddressOf())))) return false;

    m_impl->capture = FindCaptureFilter(device.name);
    if (!m_impl->capture) { DLog(L"capture device not found by name: " + device.name); return false; }
    if (FAILED(m_impl->graph->AddFilter(m_impl->capture.Get(), L"Capture"))) {
        DLog(L"AddFilter(capture) failed"); return false;
    }

    ComPtr<IPin> outPin = GetPinByDir(m_impl->capture.Get(), PINDIR_OUTPUT);
    if (!outPin) { DLog(L"capture output pin not found"); return false; }

    AM_MEDIA_TYPE* negMt = ConfigureNV12(outPin.Get());
    if (!negMt) { DLog(L"could not negotiate NV12 1080p on capture pin"); return false; }

    // Publish the negotiated format for the renderer.
    if (ValidNV12Type(negMt)) {
        VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)negMt->pbFormat;
        m_format.width  = (uint32_t)vih->bmiHeader.biWidth;
        m_format.height = (uint32_t)std::abs(static_cast<int64_t>(vih->bmiHeader.biHeight));
        ReadFrameRateFromInterval(vih->AvgTimePerFrame);
        m_format.stride = m_format.width;          // NV12: Y plane stride == width
        m_format.subtype = MFVideoFormat_NV12;
        m_format.topDown = true;
        m_format.fullRange = false;
    }

    // Build and add the sink (refcount starts at 1; sinkBase takes that ref).
    m_impl->sink = new SinkFilter(m_impl.get());
    m_impl->sinkBase.Attach(static_cast<IBaseFilter*>(m_impl->sink));
    m_impl->sink->m_pin.SetOfferType(negMt);
    if (!m_impl->sink->m_pin.m_hasOffer) { DeleteMT(negMt); return false; }

    if (FAILED(m_impl->graph->AddFilter(m_impl->sinkBase.Get(), L"NitLinkSink"))) {
        DLog(L"AddFilter(sink) failed"); DeleteMT(negMt); return false;
    }

    // One hop, no intermediate filters: capture output pin -> sink input pin.
    HRESULT hr = m_impl->graph->ConnectDirect(outPin.Get(),
                                              static_cast<IPin*>(&m_impl->sink->m_pin), negMt);
    DeleteMT(negMt);
    if (FAILED(hr)) {
        std::wstringstream ss; ss << L"ConnectDirect (capture->sink) failed 0x" << std::hex << hr;
        DLog(ss.str());
        return false;
    }

    std::wstringstream ok;
    ok << L"connected capture->sink (1 hop), " << m_format.width << L"x" << m_format.height
       << L" @ " << m_format.fps << L"fps NV12";
    DLog(ok.str());
    // Keep the graph's default reference clock; do not SetSyncSource(null).
    return true;
}

bool DShowCapture::StartCapture(FrameCallback callback) {
    if (!m_impl || !m_impl->control) return false;
    { std::lock_guard<std::mutex> lk(m_impl->cbMutex); m_impl->cb = std::move(callback); }
    HRESULT hr = m_impl->control->Run();
    if (FAILED(hr)) { std::wstringstream ss; ss << L"IMediaControl::Run failed 0x" << std::hex << hr; DLog(ss.str()); return false; }
    m_capturing = true;
    DLog(L"graph running");
    return true;
}

void DShowCapture::StopCapture() {
    if (m_impl) {
        if (m_impl->control) m_impl->control->Stop();
        { std::lock_guard<std::mutex> lk(m_impl->cbMutex); m_impl->cb = nullptr; }
    }
    m_capturing = false;
}

void DShowCapture::Close() {
    if (m_impl) {
        if (m_impl->control) m_impl->control->Stop();
        // Stop() drains the streaming thread; null the callback under the mutex
        // too so no in-flight OnSample can fire into a freed FrameBuffer.
        { std::lock_guard<std::mutex> lk(m_impl->cbMutex); m_impl->cb = nullptr; }
        if (m_impl->graph) {
            if (m_impl->sinkBase) m_impl->graph->RemoveFilter(m_impl->sinkBase.Get());
            if (m_impl->capture)  m_impl->graph->RemoveFilter(m_impl->capture.Get());
        }
        m_impl.reset();
    }
    m_capturing = false;
}

} // namespace NitLink
