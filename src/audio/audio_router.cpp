#include "audio_router.h"
#include "capture_endpoint_match.h"
#include <functiondiscoverykeys_devpkey.h>
#include <cfgmgr32.h>
#include <propvarutil.h>
#include <mmreg.h>
#include <avrt.h>
#include <debugapi.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <memory>
#include <sstream>

#include <initguid.h>
#include <devpkey.h>

#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Avrt.lib")
#pragma comment(lib, "Cfgmgr32.lib")

namespace NitLink {

// Container ID of a device node; GUID_NULL when it has none.
static GUID DevNodeContainerId(DEVINST devInst)
{
    GUID container = GUID_NULL;
    DEVPROPTYPE type = 0;
    ULONG size = sizeof(container);
    if (CM_Get_DevNode_PropertyW(devInst, &DEVPKEY_Device_ContainerId, &type,
                                 reinterpret_cast<PBYTE>(&container), &size, 0) != CR_SUCCESS ||
        type != DEVPROP_TYPE_GUID) {
        return GUID_NULL;
    }
    return container;
}

// Container ID of the physical device behind a device-interface path (the
// video capture device's symbolic link). Windows gives every function of
// one USB device (video, audio, HID) the same container ID.
static GUID ContainerIdForInterface(const std::wstring& interfacePath)
{
    if (interfacePath.empty()) return GUID_NULL;
    wchar_t instanceId[MAX_DEVICE_ID_LEN] = {};
    DEVPROPTYPE type = 0;
    ULONG size = sizeof(instanceId);
    if (CM_Get_Device_Interface_PropertyW(interfacePath.c_str(), &DEVPKEY_Device_InstanceId,
                                          &type, reinterpret_cast<PBYTE>(instanceId),
                                          &size, 0) != CR_SUCCESS ||
        type != DEVPROP_TYPE_STRING) {
        return GUID_NULL;
    }
    DEVINST devInst = 0;
    if (CM_Locate_DevNodeW(&devInst, instanceId, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
        return GUID_NULL;
    }
    return DevNodeContainerId(devInst);
}

// The PC's own container ID (the root device node's), shared by every
// built-in device and therefore never a pairing signal.
static GUID ComputerContainerId()
{
    DEVINST root = 0;
    if (CM_Locate_DevNodeW(&root, nullptr, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
        return GUID_NULL;
    }
    return DevNodeContainerId(root);
}

static std::wstring GuidText(const GUID& g)
{
    wchar_t text[64] = {};
    StringFromGUID2(g, text, 64);
    return text;
}

// Reference time helpers (Windows uses 100-ns units)
static constexpr REFERENCE_TIME REFTIMES_PER_SEC      = 10000000;
static constexpr REFERENCE_TIME REFTIMES_PER_MILLISEC = 10000;

// How long to wait before re-attempting an endpoint that failed to open.
static constexpr int kRetryIntervalMs = 500;

// Fixed allocation bounds pathological packet bursts; configured targets and
// emergency trimming keep normal latency far below this capacity.
static constexpr int   kFifoCapacityMs  = 400;

static constexpr int   kStatsIntervalMs = 5000;
static constexpr DWORD kPumpWaitMs      = 200;

// KSDATAFORMAT_SUBTYPE_* live in ksmedia.h, which drags in the whole kernel
// streaming header chain. These two are the only ones the router needs.
static const GUID kSubtypeIeeeFloat =
    {0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
static const GUID kSubtypePcm =
    {0x00000001,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
// Stands in for GUID_NULL, which would pull in a uuid.lib dependency for one
// sentinel value.
static const GUID kSubtypeUnknown = {0,0,0,{0,0,0,0,0,0,0,0}};

static void AudioLog(const std::wstring& msg) {
    OutputDebugStringW((L"[NitLink/Audio] " + msg + L"\n").c_str());
}

struct LowPeriodResult {
    HRESULT hr = E_NOTIMPL;
    const wchar_t* stage = L"activate IAudioClient3";
    UINT32 minimum = 0, normal = 0, requested = 0, selected = 0;
};

// Query the engine using its native mix format on render. A new client is
// used for each attempt; a failed Initialize never gets reused.
static bool TryLowPeriod(IMMDevice* device, const WAVEFORMATEX* format,
                         HANDLE event, int requestedMs, ComPtr<IAudioClient>& client,
                         UINT32& periodFrames, LowPeriodResult* result = nullptr) {
    LowPeriodResult d;
    const auto finish = [&](HRESULT hr, const wchar_t* stage) {
        d.hr=hr; d.stage=stage; if (result) *result=d; return SUCCEEDED(hr);
    };
    ComPtr<IAudioClient3> low;
    HRESULT hr=device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void**>(low.GetAddressOf()));
    if (FAILED(hr)) return finish(hr,L"activate IAudioClient3");
    UINT32 fundamental=0, maximum=0;
    hr=low->GetSharedModeEnginePeriod(format,&d.normal,&fundamental,&d.minimum,&maximum);
    if (FAILED(hr)) return finish(hr,L"query native engine period");
    d.requested=UINT32(uint64_t(format->nSamplesPerSec)*requestedMs/1000);
    d.selected=AudioConvert::SelectPeriod(d.requested,fundamental,d.minimum,maximum);
    if (!d.selected) return finish(E_INVALIDARG,L"invalid driver period range");
    hr=low->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK,d.selected,format,nullptr);
    if (FAILED(hr)) return finish(hr,L"initialize native short-period stream");
    hr=low->SetEventHandle(event);
    if (FAILED(hr)) return finish(hr,L"set event handle");
    hr=low.As(&client);
    if (FAILED(hr)) return finish(hr,L"get audio client");
    periodFrames=d.selected;
    WAVEFORMATEX* current=nullptr;
    UINT32 currentPeriod=0;
    if (SUCCEEDED(low->GetCurrentSharedModeEnginePeriod(&current,&currentPeriod)) && current &&
        current->nSamplesPerSec==format->nSamplesPerSec && currentPeriod) periodFrames=currentPeriod;
    CoTaskMemFree(current);
    return finish(S_OK,L"native short-period stream active");
}

static std::wstring HrString(HRESULT hr) {
    std::wstringstream ss;
    ss << L"0x" << std::hex << (unsigned long)hr;
    return ss.str();
}

static std::wstring DescribeFormat(const WAVEFORMATEX* fmt) {
    if (!fmt) return L"(none)";
    std::wstringstream ss;
    ss << fmt->nSamplesPerSec << L" Hz, " << fmt->nChannels << L"ch, "
       << fmt->wBitsPerSample << L"-bit";
    return ss.str();
}

// Resolves the effective sample encoding, unwrapping WAVE_FORMAT_EXTENSIBLE.
static GUID EffectiveSubFormat(const WAVEFORMATEX* fmt) {
    if (!fmt) return kSubtypeUnknown;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22) {
        return reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt)->SubFormat;
    }
    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return kSubtypeIeeeFloat;
    if (fmt->wFormatTag == WAVE_FORMAT_PCM)        return kSubtypePcm;
    return kSubtypeUnknown;
}

// Shared-mode linear audio must describe the same bytes that the copy and
// volume loops consume. Driver metadata does not set allocation sizes directly.
static bool ValidWaveFormat(const WAVEFORMATEX* fmt) {
    if (!fmt || fmt->nChannels == 0 || fmt->nChannels > 32 ||
        fmt->nSamplesPerSec < 8000 || fmt->nSamplesPerSec > 768000 ||
        (fmt->wBitsPerSample != 8 && fmt->wBitsPerSample != 16 &&
         fmt->wBitsPerSample != 24 && fmt->wBitsPerSample != 32 && fmt->wBitsPerSample != 64)) return false;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        if (fmt->cbSize != sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) return false;
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
        if (ext->Samples.wValidBitsPerSample > fmt->wBitsPerSample) return false;
    } else if (fmt->cbSize != 0) return false;
    const GUID subtype = EffectiveSubFormat(fmt);
    if (subtype != kSubtypePcm && subtype != kSubtypeIeeeFloat) return false;
    if (subtype == kSubtypeIeeeFloat && fmt->wBitsPerSample != 32 && fmt->wBitsPerSample != 64) return false;
    const uint32_t align = uint32_t(fmt->nChannels) * (fmt->wBitsPerSample / 8);
    return fmt->nBlockAlign == align && fmt->nAvgBytesPerSec == fmt->nSamplesPerSec * align;
}

static WAVEFORMATEX* CloneWaveFormat(const WAVEFORMATEX* src) {
    if (!ValidWaveFormat(src)) return nullptr;
    const size_t bytes = sizeof(WAVEFORMATEX) + src->cbSize;
    WAVEFORMATEX* dst = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(bytes));
    if (!dst) return nullptr;
    memcpy(dst, src, bytes);
    return dst;
}

static bool SameWaveFormat(const WAVEFORMATEX* a, const WAVEFORMATEX* b) {
    if (!a || !b) return false;
    if (a->nChannels      != b->nChannels)      return false;
    if (a->nSamplesPerSec != b->nSamplesPerSec) return false;
    if (a->wBitsPerSample != b->wBitsPerSample) return false;
    if (a->nBlockAlign    != b->nBlockAlign)    return false;
    return IsEqualGUID(EffectiveSubFormat(a), EffectiveSubFormat(b)) != 0;
}

static AudioConvert::Format PcmDescription(const WAVEFORMATEX* fmt) {
    AudioConvert::Format f;
    if (!ValidWaveFormat(fmt)) return f;
    f.channels=fmt->nChannels; f.bits=fmt->wBitsPerSample; f.validBits=f.bits;
    f.floating=EffectiveSubFormat(fmt)==kSubtypeIeeeFloat;
    if (fmt->wFormatTag==WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext=reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
        if (ext->Samples.wValidBitsPerSample) f.validBits=ext->Samples.wValidBitsPerSample;
        f.mask=ext->dwChannelMask;
    }
    return f;
}

static bool ScaleInPlace(BYTE* data, UINT32 frames, const WAVEFORMATEX* fmt, float vol) {
    const auto f=PcmDescription(fmt);
    if (!data || !AudioConvert::Valid(f) || !std::isfinite(vol)) return false;
    const size_t count=size_t(frames)*f.channels, bytes=f.bits/8;
    for (size_t i=0;i<count;++i) AudioConvert::Write(data+i*bytes,f,AudioConvert::Read(data+i*bytes,f)*vol);
    return true;
}

// ---------------------------------------------------------------------------
// Endpoint change notifications
// ---------------------------------------------------------------------------

// Watches for the events that invalidate an open IAudioClient: the default
// playback device changing (headset swap, or a vendor control panel toggling
// its virtual surround endpoint in and out), and a tracked device being
// disabled or unplugged. Callbacks arrive on an MMDevice system thread, so
// they only set flags; the worker does the actual rebuild.
class AudioRouter::EndpointNotifier : public IMMNotificationClient {
public:
    explicit EndpointNotifier(AudioRouter* owner) : m_owner(owner) {}

    // Called before Release so a late callback cannot touch a dead router.
    void Detach() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_owner = nullptr;
    }

    void SetDeviceIds(const std::wstring& captureId, const std::wstring& renderId) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_captureId = captureId;
        m_renderId  = renderId;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return (ULONG)InterlockedIncrement(&m_ref);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = (ULONG)InterlockedDecrement(&m_ref);
        if (n == 0) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(IMMNotificationClient))) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_owner) {
                m_owner->m_restartRender = true;
                AudioLog(L"Notify: default render endpoint changed");
            }
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD newState) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_owner || !id) return S_OK;

        if (!m_captureId.empty() && m_captureId == id) {
            if (newState != DEVICE_STATE_ACTIVE) {
                m_owner->m_restartCapture = true;
                AudioLog(L"Notify: capture endpoint left the active state");
            }
        } else if (!m_renderId.empty() && m_renderId == id) {
            if (newState != DEVICE_STATE_ACTIVE) {
                m_owner->m_restartRender = true;
                AudioLog(L"Notify: render endpoint left the active state");
            }
        } else if (newState == DEVICE_STATE_ACTIVE && m_captureId.empty()) {
            // A device came back while the capture side is down: worth a
            // re-scan, since this is the card being plugged back in.
            m_owner->m_restartCapture = true;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_owner && m_captureId.empty()) m_owner->m_restartCapture = true;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_owner || !id) return S_OK;
        if (!m_captureId.empty() && m_captureId == id) m_owner->m_restartCapture = true;
        if (!m_renderId.empty()  && m_renderId  == id) m_owner->m_restartRender  = true;
        return S_OK;
    }

    // Ignored on purpose. An in-place format change (Sound control panel, or a
    // driver reconfiguring its mix format) invalidates the client, and the
    // AUDCLNT_E_DEVICE_INVALIDATED check in the pump catches that within one
    // poll interval without needing to filter property keys here.
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
        return S_OK;
    }

private:
    LONG         m_ref = 1;
    std::mutex   m_mutex;
    AudioRouter* m_owner = nullptr;
    std::wstring m_captureId;
    std::wstring m_renderId;
};

// ---------------------------------------------------------------------------
// AudioRouter
// ---------------------------------------------------------------------------

AudioRouter::AudioRouter()
{
    m_captureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_renderEvent  = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_stopEvent    = CreateEventW(nullptr, TRUE,  FALSE, nullptr);
}

AudioRouter::~AudioRouter()
{
    Shutdown();
    if (m_captureEvent) CloseHandle(m_captureEvent);
    if (m_renderEvent)  CloseHandle(m_renderEvent);
    if (m_stopEvent)    CloseHandle(m_stopEvent);
}

bool AudioRouter::Initialize(const std::wstring& nameHint,
                             const std::wstring& videoSymbolicLink)
{
    AudioLog(L"Initialize: begin");

    // Re-initialising over a live router is legal (SwitchCaptureDevice does
    // exactly that); make sure the previous worker is gone first.
    Shutdown();

    m_videoSymbolicLink = videoSymbolicLink;
    m_videoContainer = ContainerIdForInterface(videoSymbolicLink);
    m_computerContainer = ComputerContainerId();
    m_containerAmbiguityLogged = false;
    AudioLog(L"Initialize: video device container " + GuidText(m_videoContainer) +
             (IsPairableContainer(m_videoContainer, m_computerContainer)
                  ? L" (usable for audio pairing)" : L" (not usable for audio pairing)"));

    m_focusGate = {};
    m_exclusivePermitted = false;
    m_exclusiveFailure.clear();
    m_nameHint = nameHint.empty() ? std::wstring(L"Elgato") : nameHint;
    m_setupFailureLogged = false;
    {
        std::lock_guard<std::mutex> lock(m_startMutex);
        m_startAttempted = false;
        m_startSucceeded = false;
    }

    m_lastCaptureError = S_OK;
    if (m_stopEvent) ResetEvent(m_stopEvent);
    m_running = true;
    m_thread = std::thread(&AudioRouter::RouteLoop, this);

    // Wait for the worker's first open attempt so callers keep the original
    // "Initialize() reports whether audio came up" contract. Either way the
    // worker stays alive and keeps retrying in the background.
    std::unique_lock<std::mutex> lock(m_startMutex);
    m_startCv.wait_for(lock, std::chrono::seconds(3),
                       [this] { return m_startAttempted; });
    return m_startSucceeded;
}

void AudioRouter::Shutdown()
{
    const bool wasRunning = m_running.exchange(false);
    if (m_stopEvent) SetEvent(m_stopEvent);
    if (m_thread.joinable()) {
        if (wasRunning) AudioLog(L"Shutdown: stopping");
        m_thread.join();
    }
    m_streaming = false;
}

bool AudioRouter::FindCaptureDevice(const std::wstring& nameHint, ComPtr<IMMDevice>& outDevice)
{
    // A container lookup that failed in Initialize (device interface not
    // ready yet, for example) is retried on every setup and recovery attempt
    // instead of keeping the empty ID.
    if (IsEqualGUID(m_videoContainer, GUID_NULL) && !m_videoSymbolicLink.empty()) {
        m_videoContainer = ContainerIdForInterface(m_videoSymbolicLink);
        if (!IsEqualGUID(m_videoContainer, GUID_NULL)) {
            AudioLog(L"Video device container resolved on retry: " + GuidText(m_videoContainer));
        }
    }
    if (IsEqualGUID(m_computerContainer, GUID_NULL)) {
        m_computerContainer = ComputerContainerId();
    }

    ComPtr<IMMDeviceCollection> collection;
    HRESULT hr = m_enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) return false;

    UINT count = 0;
    collection->GetCount(&count);

    std::vector<ComPtr<IMMDevice>> devices;
    std::vector<CaptureEndpointInfo> endpoints;
    for (UINT i = 0; i < count; i++) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) continue;

        ComPtr<IPropertyStore> props;
        if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) continue;

        CaptureEndpointInfo info;
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &value)) &&
            value.vt == VT_LPWSTR && value.pwszVal) {
            info.friendlyName = value.pwszVal;
        }
        PropVariantClear(&value);
        if (SUCCEEDED(props->GetValue(PKEY_Device_ContainerId, &value)) &&
            value.vt == VT_CLSID && value.puuid) {
            info.containerId = *value.puuid;
        }
        PropVariantClear(&value);

        devices.push_back(device);
        endpoints.push_back(std::move(info));
    }

    const CaptureEndpointChoice choice = ChooseCaptureEndpoint(
        endpoints, nameHint, m_videoContainer, m_computerContainer);
    if (choice.index < 0) {
        // Logged once per Initialize: the worker retries this every interval.
        if (!choice.ambiguousContainerMatches.empty() && !m_containerAmbiguityLogged) {
            std::wstring names;
            for (int i : choice.ambiguousContainerMatches) {
                if (!names.empty()) names += L"; ";
                names += endpoints[static_cast<size_t>(i)].friendlyName;
            }
            AudioLog(L"Not pairing by container: " +
                     std::to_wstring(choice.ambiguousContainerMatches.size()) +
                     L" capture endpoints share the video device's container (" +
                     names + L")");
            m_containerAmbiguityLogged = true;
        }
        return false;
    }
    outDevice = devices[static_cast<size_t>(choice.index)];
    AudioLog(std::wstring(L"Matched capture device") +
             (choice.byName ? L": " : L" on the same physical device as the video: ") +
             endpoints[static_cast<size_t>(choice.index)].friendlyName);
    return true;
}

bool AudioRouter::SetupCapture()
{
    ComPtr<IMMDevice> captureDevice;
    if (!FindCaptureDevice(m_nameHint, captureDevice)) return false;

    LPWSTR id = nullptr;
    if (SUCCEEDED(captureDevice->GetId(&id)) && id) {
        m_captureDeviceId = id;
        CoTaskMemFree(id);
    }

    HRESULT hr = captureDevice->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        (void**)m_captureClient.ReleaseAndGetAddressOf());
    if (FAILED(hr)) { AudioLog(L"capture Activate failed " + HrString(hr)); m_lastCaptureError = hr; return false; }

    hr = m_captureClient->GetMixFormat(&m_captureFormat);
    if (FAILED(hr) || !ValidWaveFormat(m_captureFormat)) { AudioLog(L"capture GetMixFormat failed " + HrString(hr)); return false; }

    UINT32 capturePeriod = 0;
    if (!TryLowPeriod(captureDevice.Get(), m_captureFormat, m_captureEvent, 3,
                      m_captureClient, capturePeriod)) {
    // Capacity for delayed scheduling; DrainCapture consumes packets immediately.
    hr = m_captureClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        100 * REFTIMES_PER_MILLISEC,
        0,
        m_captureFormat,
        nullptr);
    if (FAILED(hr)) { AudioLog(L"capture Initialize failed " + HrString(hr)); m_lastCaptureError = hr; return false; }
    hr = m_captureClient->SetEventHandle(m_captureEvent);
    if (FAILED(hr)) { AudioLog(L"capture SetEventHandle failed " + HrString(hr)); return false; }
    }
    if (!FifoReset(m_captureFormat->nBlockAlign, m_captureFormat->nSamplesPerSec)) return false;

    hr = m_captureClient->GetBufferSize(&m_captureBufferFrames);
    if (FAILED(hr) || !m_captureBufferFrames || m_captureBufferFrames > m_captureFormat->nSamplesPerSec * 2) return false;

    hr = m_captureClient->GetService(IID_PPV_ARGS(&m_captureService));
    if (FAILED(hr)) return false;

    hr = m_captureClient->Start();
    if (FAILED(hr)) { AudioLog(L"captureClient->Start failed " + HrString(hr)); return false; }

    m_lastCaptureError = S_OK;
    AudioLog(L"Capture format: " + DescribeFormat(m_captureFormat));
    return true;
}

struct ExclusiveResult {
    HRESULT hr = E_NOTIMPL;
    const wchar_t* stage = L"activate exclusive client";
    REFERENCE_TIME minimum = 0, requested = 0;
    UINT32 frames = 0;
    bool aligned = false;
};

// A failed Initialize client is discarded before the alignment retry.
static bool OpenExclusive(IMMDevice* device, const WAVEFORMATEX* format, HANDLE event,
                          ComPtr<IAudioClient>& client, ExclusiveResult& d) {
    const auto step=[&](HRESULT hr, const wchar_t* stage) { d.hr=hr; d.stage=stage; return SUCCEEDED(hr); };
    const auto activate=[&]() {
        return device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,
            reinterpret_cast<void**>(client.ReleaseAndGetAddressOf()));
    };
    if (!step(activate(),L"activate exclusive client")) return false;
    REFERENCE_TIME normal=0;
    if (!step(client->GetDevicePeriod(&normal,&d.minimum),L"query exclusive minimum")) return false;
    if (d.minimum<=0 || d.minimum>2*REFTIMES_PER_SEC)
        return step(E_INVALIDARG,L"invalid exclusive minimum");
    d.requested=d.minimum;
    HRESULT hr=client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        d.requested,d.requested,format,nullptr);
    if (hr==AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 alignedFrames=0;
        if (!step(client->GetBufferSize(&alignedFrames),L"query aligned exclusive buffer")) return false;
        if (!alignedFrames || alignedFrames>format->nSamplesPerSec*2)
            return step(E_INVALIDARG,L"invalid aligned exclusive buffer");
        // Round to the nearest 100 ns, as required by the WASAPI alignment recipe.
        d.requested=REFERENCE_TIME((uint64_t(alignedFrames)*REFTIMES_PER_SEC+format->nSamplesPerSec/2)/format->nSamplesPerSec);
        d.requested=std::max(d.minimum,d.requested);
        d.aligned=true;
        if (!step(activate(),L"reactivate aligned exclusive client")) return false;
        hr=client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            d.requested,d.requested,format,nullptr);
    }
    if (!step(hr,L"initialize exclusive minimum")) return false;
    if (!step(client->SetEventHandle(event),L"set exclusive event")) return false;
    if (!step(client->GetBufferSize(&d.frames),L"query exclusive buffer")) return false;
    if (!d.frames || d.frames>format->nSamplesPerSec*2)
        return step(E_INVALIDARG,L"invalid exclusive buffer size");
    return true;
}

bool AudioRouter::SetupExclusive(IMMDevice* device, const WAVEFORMATEX* native, std::wstring& failure) {
    ExclusiveResult d;
    const auto fail=[&](HRESULT hr,const wchar_t* stage) {
        std::wstringstream text;
        text << stage << L" " << HrString(hr);
        if (d.minimum) text << L" | exclusive min " << d.minimum/10000.0 << L" ms";
        if (d.requested) text << L", requested " << d.requested/10000.0 << L" ms";
        failure=text.str(); return false;
    };
    ComPtr<IAudioClient> probe;
    HRESULT hr=device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,
        reinterpret_cast<void**>(probe.GetAddressOf()));
    if (FAILED(hr)) return fail(hr,L"activate exclusive format probe");
    std::vector<WAVEFORMATEXTENSIBLE> candidates;
    const auto append=[&](const WAVEFORMATEX* fmt) {
        if (!ValidWaveFormat(fmt)) return;
        const double ratio=double(m_captureFormat->nSamplesPerSec)/fmt->nSamplesPerSec;
        if (ratio<0.125 || ratio>8 || !AudioConvert::CanMap(PcmDescription(m_captureFormat),PcmDescription(fmt))) return;
        WAVEFORMATEXTENSIBLE copy{};
        memcpy(&copy,fmt,sizeof(WAVEFORMATEX)+fmt->cbSize);
        candidates.push_back(copy);
    };
    append(native); append(m_captureFormat);
    // Shared mix formats are often float even when the hardware only accepts PCM.
    for (DWORD rate : std::array<DWORD,5>{native->nSamplesPerSec,m_captureFormat->nSamplesPerSec,48000,44100,96000}) {
        for (WORD channels : {native->nChannels,WORD(2)}) {
            for (WORD bits : {WORD(32),WORD(24),WORD(16)}) {
                for (WORD valid : {bits,WORD(24)}) {
                    if (valid>bits || (valid!=bits && bits!=32)) continue;
                    WAVEFORMATEXTENSIBLE fmt{};
                    fmt.Format={WAVE_FORMAT_EXTENSIBLE,channels,rate,rate*channels*(bits/8),WORD(channels*(bits/8)),bits,22};
                    fmt.Samples.wValidBitsPerSample=valid;
                    fmt.dwChannelMask=channels==2 ? 3 : PcmDescription(native).mask;
                    fmt.SubFormat=kSubtypePcm;
                    append(&fmt.Format);
                    if (valid==bits && channels<=2) {
                        fmt.Format.wFormatTag=WAVE_FORMAT_PCM; fmt.Format.cbSize=0;
                        append(&fmt.Format);
                    }
                }
            }
        }
    }
    const WAVEFORMATEX* chosen=nullptr;
    for (const auto& candidate : candidates) {
        hr=probe->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,&candidate.Format,nullptr);
        if (hr==S_OK) { chosen=&candidate.Format; break; }
        if (hr!=AUDCLNT_E_UNSUPPORTED_FORMAT && hr!=S_FALSE)
            return fail(hr,L"probe exclusive format");
    }
    if (!chosen) return fail(AUDCLNT_E_UNSUPPORTED_FORMAT,L"no convertible exclusive PCM format");
    probe.Reset();
    if (!OpenExclusive(device,chosen,m_renderEvent,m_renderClient,d)) return fail(d.hr,d.stage);
    m_renderFormat=CloneWaveFormat(chosen);
    if (!m_renderFormat) return fail(E_OUTOFMEMORY,L"copy exclusive format");
    m_renderBufferFrames=m_renderPeriodFrames=d.frames;
    hr=m_renderClient->GetService(IID_PPV_ARGS(&m_renderService));
    if (FAILED(hr)) return fail(hr,L"get exclusive render service");
    m_conversionReady=PrepareConversion();
    if (!m_conversionReady) return fail(E_FAIL,L"prepare exclusive conversion");
    m_fifoHead=m_fifoBytes=0; m_fifoPrimed=false; ResetDrift();
    // Prime one WHOLE ping-pong buffer before Start, without consuming stale FIFO.
    BYTE* data=nullptr;
    hr=m_renderService->GetBuffer(m_renderBufferFrames,&data);
    if (FAILED(hr)) return fail(hr,L"prime exclusive buffer");
    hr=m_renderService->ReleaseBuffer(m_renderBufferFrames,AUDCLNT_BUFFERFLAGS_SILENT);
    if (FAILED(hr)) return fail(hr,L"release exclusive prime");
    ResetEvent(m_renderEvent);
    // Activation can change while the driver is being initialized.
    if (m_ownerWindow.load() && (!m_running.load() || !m_exclusiveRequested.load() || !OwnerIsForeground()))
        return fail(E_ABORT,L"exclusive suspended before start (window inactive or closing)");
    hr=m_renderClient->Start();
    if (FAILED(hr)) return fail(hr,L"start exclusive stream");
    SetupRenderClock();
    m_lastRenderEvent=std::chrono::steady_clock::now();
    m_periodUs=UINT32(uint64_t(d.frames)*1000000/chosen->nSamplesPerSec);
    m_effectiveQueueUs=m_periodUs.load();
    m_adaptiveSupported=true; m_exclusiveActive=true;
    REFERENCE_TIME latency=0;
    const HRESULT latencyHr=m_renderClient->GetStreamLatency(&latency);
    std::wstringstream status;
    status << L"Exclusive event | exclusive min " << d.minimum/10000.0
           << L" ms | requested " << d.requested/10000.0 << L" ms"
           << L" | actual block " << m_periodUs.load()/1000.0 << L" ms (" << d.frames << L" frames)"
           << (d.aligned ? L" | driver alignment applied" : L"")
           << L" | driver stream latency ";
    if (SUCCEEDED(latencyHr)) status << latency/10000.0 << L" ms"; else status << L"unknown";
    status << L" | capture " << DescribeFormat(m_captureFormat) << L" -> stream " << DescribeFormat(chosen);
    { std::lock_guard<std::mutex> lock(m_endpointInfoMutex); m_endpointInfo=status.str(); }
    AudioLog(status.str());
    return true;
}

bool AudioRouter::SetupRender()
{
    ComPtr<IMMDevice> device;
    HRESULT hr=m_enumerator->GetDefaultAudioEndpoint(eRender,eConsole,&device);
    if (FAILED(hr)) { AudioLog(L"GetDefaultAudioEndpoint failed "+HrString(hr)); return false; }
    LPWSTR id=nullptr;
    if (SUCCEEDED(device->GetId(&id)) && id) { m_renderDeviceId=id; CoTaskMemFree(id); }
    const auto activate=[&]() {
        return device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,
            reinterpret_cast<void**>(m_renderClient.ReleaseAndGetAddressOf()));
    };
    if (FAILED(activate())) return false;
    WAVEFORMATEX* native=nullptr;
    hr=m_renderClient->GetMixFormat(&native);
    if (FAILED(hr) || !ValidWaveFormat(native)) { CoTaskMemFree(native); return false; }
    const auto freeNative=[](WAVEFORMATEX* p) { CoTaskMemFree(p); };
    std::unique_ptr<WAVEFORMATEX,decltype(freeNative)> nativeOwner(native,freeNative);
    std::wstring exclusiveFailure;
    if (m_exclusiveRequested.load() && m_exclusivePermitted.load()) {
        if (m_failedExclusiveDeviceId!=m_renderDeviceId) m_exclusiveFailure.clear();
        exclusiveFailure=m_exclusiveFailure;
        if (exclusiveFailure.empty()) {
            // Release the mix-format query client before taking exclusive access.
            m_renderClock.Reset(); m_renderClockFrequency=0;
            m_renderClient.Reset();
            if (SetupExclusive(device.Get(),native,exclusiveFailure)) return true;
            const auto deviceId=m_renderDeviceId;
            TeardownRender(); // release a partly initialized exclusive stream before shared fallback
            m_renderDeviceId=deviceId;
            if (!m_running.load()) return false;
            if (FAILED(activate())) return false;
        }
        AudioLog(L"Exclusive fallback: "+exclusiveFailure);
    }
    const double rateRatio=double(m_captureFormat->nSamplesPerSec)/native->nSamplesPerSec;
    const bool nativeConvertible=AudioConvert::CanMap(PcmDescription(m_captureFormat),PcmDescription(native)) &&
        rateRatio>=0.125 && rateRatio<=8;
    LowPeriodResult probe;
    m_renderPeriodFrames=0;
    bool lowPeriod=false;
    if (nativeConvertible) {
        lowPeriod=TryLowPeriod(device.Get(),native,m_renderEvent,
            std::max(1,m_renderQueueTargetMs.load()/2),m_renderClient,m_renderPeriodFrames,&probe);
    } else {
        probe.hr=AUDCLNT_E_UNSUPPORTED_FORMAT;
        probe.stage=L"native channel layout / PCM encoding / rate ratio is not supported by converter";
        // Still report driver capability even when conversion cannot use it.
        ComPtr<IAudioClient3> query;
        if (SUCCEEDED(m_renderClient.As(&query))) {
            UINT32 fundamental=0, maximum=0;
            query->GetSharedModeEnginePeriod(native,&probe.normal,&fundamental,&probe.minimum,&maximum);
        }
    }
    if (lowPeriod) {
        m_renderFormat=CloneWaveFormat(native);
    } else {
        AudioLog(std::wstring(L"Low-period fallback: ")+probe.stage+L" "+HrString(probe.hr));
        if (FAILED(activate())) return false;
        hr=m_renderClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,100*REFTIMES_PER_MILLISEC,0,m_captureFormat,nullptr);
        if (FAILED(hr)) {
            // Legacy no-conversion retry is safe only for an identical format.
            if (!SameWaveFormat(m_captureFormat,native) || FAILED(activate())) return false;
            hr=m_renderClient->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                100*REFTIMES_PER_MILLISEC,0,m_captureFormat,nullptr);
        }
        if (FAILED(hr)) { AudioLog(L"render Initialize failed "+HrString(hr)); return false; }
        m_renderFormat=CloneWaveFormat(m_captureFormat);
        if (!m_renderFormat) return false;
        if (FAILED(m_renderClient->SetEventHandle(m_renderEvent))) return false;
        REFERENCE_TIME period=0;
        if (SUCCEEDED(m_renderClient->GetDevicePeriod(&period,nullptr)))
            m_renderPeriodFrames=UINT32((uint64_t(period)*m_renderFormat->nSamplesPerSec+REFTIMES_PER_SEC-1)/REFTIMES_PER_SEC);
        if (!m_renderPeriodFrames) m_renderPeriodFrames=m_renderFormat->nSamplesPerSec/100;
    }
    if (!m_renderFormat) return false;
    hr=m_renderClient->GetBufferSize(&m_renderBufferFrames);
    if (FAILED(hr) || !m_renderBufferFrames || m_renderBufferFrames>m_renderFormat->nSamplesPerSec*2) return false;
    hr=m_renderClient->GetService(IID_PPV_ARGS(&m_renderService));
    if (FAILED(hr)) return false;
    m_conversionReady=PrepareConversion();
    // Native format cannot be copied if converter allocation/setup failed.
    if (lowPeriod && !m_conversionReady) { AudioLog(L"native format converter setup failed"); return false; }
    m_adaptiveSupported=m_conversionReady;
    m_periodUs=UINT32(uint64_t(m_renderPeriodFrames)*1000000/m_renderFormat->nSamplesPerSec);
    std::wstringstream status;
    status << (lowPeriod ? L"Native shared low-period" : L"Compatibility shared (Windows conversion)")
           << L" | native min " << (probe.minimum ? std::to_wstring(double(probe.minimum)*1000/native->nSamplesPerSec) : L"unknown")
           << L" ms, default " << (probe.normal ? std::to_wstring(double(probe.normal)*1000/native->nSamplesPerSec) : L"unknown")
           << L" ms | actual " << m_periodUs.load()/1000.0 << L" ms"
           << L" | capture " << DescribeFormat(m_captureFormat) << L" -> stream " << DescribeFormat(m_renderFormat);
    if (m_exclusiveRequested.load() && !m_exclusivePermitted.load())
        status << L" | exclusive suspended: window inactive, hidden, minimized or regaining focus";
    if (!exclusiveFailure.empty()) status << L" | exclusive fallback: " << exclusiveFailure;
    if (!lowPeriod) status << L" | fallback: " << probe.stage << L" " << HrString(probe.hr);
    { std::lock_guard<std::mutex> lock(m_endpointInfoMutex); m_endpointInfo=status.str(); }
    AudioLog(status.str());
    m_fifoHead=m_fifoBytes=0; m_fifoPrimed=false;
    ResetDrift();
    SetupRenderClock();
    hr=m_renderClient->Start();
    if (FAILED(hr)) { AudioLog(L"render Start failed "+HrString(hr)); return false; }
    return true;
}

std::wstring AudioRouter::EndpointInfo() const {
    std::lock_guard<std::mutex> lock(m_endpointInfoMutex);
    return m_endpointInfo;
}

bool AudioRouter::PrepareConversion() {
    m_inputPcm=PcmDescription(m_captureFormat);
    m_outputPcm=PcmDescription(m_renderFormat);
    if (!AudioConvert::CanMap(m_inputPcm,m_outputPcm)) return false;
    return m_sinc.Initialize(double(m_captureFormat->nSamplesPerSec)/m_renderFormat->nSamplesPerSec);
}

void AudioRouter::TeardownCapture()
{
    if (m_captureClient) m_captureClient->Stop();
    m_captureService.Reset();
    m_captureClient.Reset();
    if (m_captureFormat) { CoTaskMemFree(m_captureFormat); m_captureFormat = nullptr; }
    m_captureBufferFrames = 0;
    m_captureDeviceId.clear();
}

void AudioRouter::TeardownRender()
{
    if (m_renderClient) m_renderClient->Stop();
    m_renderService.Reset();
    m_renderClock.Reset(); m_renderClockFrequency=0;
    m_renderClient.Reset();
    if (m_renderFormat) { CoTaskMemFree(m_renderFormat); m_renderFormat = nullptr; }
    m_renderBufferFrames = 0;
    m_queueMs = 0;
    m_periodUs = 0;
    m_adaptiveSupported = false;
    m_conversionReady = false;
    m_exclusiveActive = false;
    m_queueSnapshot = 0; m_effectiveQueueUs = 0;
    m_fifoAverageUs = 0; m_fifoMinUs = 0; m_fifoMaxUs = 0;
    { std::lock_guard<std::mutex> lock(m_endpointInfoMutex); m_endpointInfo=L"Waiting for playback endpoint"; }
    m_renderDeviceId.clear();
}

bool AudioRouter::EnsureEndpoints()
{
    if (!m_enumerator) {
        HRESULT hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&m_enumerator));
        if (FAILED(hr)) {
            if (!m_setupFailureLogged) {
                AudioLog(L"failed to create device enumerator " + HrString(hr));
                m_setupFailureLogged = true;
            }
            return false;
        }

        auto* notifier = new (std::nothrow) EndpointNotifier(this);
        if (notifier) {
            if (SUCCEEDED(m_enumerator->RegisterEndpointNotificationCallback(notifier))) {
                m_notifier.Attach(notifier); // takes the constructor's reference
            } else {
                notifier->Release();
                AudioLog(L"endpoint notifications unavailable; relying on stream errors");
            }
        }
    }

    // The render client is opened against the capture format, so a capture
    // rebuild forces a render rebuild too.
    if (m_restartCapture.exchange(false)) {
        TeardownCapture();
        TeardownRender();
    }
    if (m_retryExclusive.exchange(false)) m_exclusiveFailure.clear();
    if (m_restartRender.exchange(false)) {
        TeardownRender();
    }

    if (!m_captureService) {
        TeardownRender();
        if (!SetupCapture()) {
            TeardownCapture();
            if (!m_setupFailureLogged) {
                AudioLog(L"capture endpoint unavailable; retrying");
                m_setupFailureLogged = true;
            }
            return false;
        }
    }

    if (!m_renderService) {
        if (!SetupRender()) {
            TeardownRender();
            if (!m_setupFailureLogged) {
                AudioLog(L"render endpoint unavailable; retrying");
                m_setupFailureLogged = true;
            }
            return false;
        }
    }

    if (m_notifier) {
        static_cast<EndpointNotifier*>(m_notifier.Get())
            ->SetDeviceIds(m_captureDeviceId, m_renderDeviceId);
    }

    if (m_setupFailureLogged) {
        AudioLog(L"endpoints recovered; routing resumed");
        m_setupFailureLogged = false;
    }
    return true;
}

void AudioRouter::HandleStreamError(HRESULT hr, const wchar_t* what, bool captureSide)
{
    // Every one of these means the client is dead and has to be rebuilt.
    // Previously they were swallowed, which is why a headset change or a
    // device-format edit silenced the app for the rest of the run.
    const bool fatal = (hr == AUDCLNT_E_DEVICE_INVALIDATED) ||
                       (hr == AUDCLNT_E_SERVICE_NOT_RUNNING) ||
                       (hr == AUDCLNT_E_NOT_INITIALIZED) ||
                       (hr == AUDCLNT_E_RESOURCES_INVALIDATED);

    AudioLog(std::wstring(what) + L" failed " + HrString(hr) +
             (fatal ? L"; rebuilding endpoint" : L""));

    if (!captureSide && m_exclusiveActive.load()) {
        m_exclusiveFailure=std::wstring(what)+L" "+HrString(hr);
        m_failedExclusiveDeviceId=m_renderDeviceId;
        m_restartRender=true;
        return;
    }
    if (!fatal) return;
    if (captureSide) m_restartCapture = true;
    else             m_restartRender = true;
}

void AudioRouter::RouteLoop()
{
    // The endpoints are created and used entirely on this thread, inside the
    // MTA. main.cpp puts the UI thread in an STA because WebView2 requires it;
    // building the WASAPI clients there and calling them from here would be a
    // cross-apartment call on interfaces that were never marshalled.
    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hrCom)) {
        AudioLog(L"CoInitializeEx failed " + HrString(hrCom));
        {
            std::lock_guard<std::mutex> lock(m_startMutex);
            m_startAttempted = true;
            m_startSucceeded = false;
        }
        m_startCv.notify_all();
        return;
    }

    // Boost thread priority for low-latency audio. The Windows pro-audio
    // task is the standard for this kind of work; it moves the thread out of
    // regular scheduling and onto the audio-thread cohort.
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    bool firstAttemptReported = false;

    while (m_running) {
        // Run on the audio worker, even while the UI is in a modal move/size loop.
        UpdateExclusiveFocus(OwnerIsForeground(),std::chrono::steady_clock::now());
        const bool ready = EnsureEndpoints();

        if (!firstAttemptReported) {
            {
                std::lock_guard<std::mutex> lock(m_startMutex);
                m_startAttempted = true;
                m_startSucceeded = ready;
            }
            m_startCv.notify_all();
            firstAttemptReported = true;
        }

        if (!ready) {
            m_streaming = false;
            WaitForSingleObject(m_stopEvent,kRetryIntervalMs);
            continue;
        }
        m_streaming = true;

        // Sleep until either endpoint has something to do, or the stop event
        // fires. Always drain capture. Shared output may be topped up on
        // either wake; exclusive output writes exactly one full block only
        // when its own render event is signaled.
        HANDLE waits[3] = { m_stopEvent, m_captureEvent, m_renderEvent };
        const DWORD wake = WaitForMultipleObjects(3, waits, FALSE, kPumpWaitMs);
        if (wake == WAIT_OBJECT_0) break;
        if (wake == WAIT_FAILED) {
            AudioLog(L"WaitForMultipleObjects failed; retrying");
            WaitForSingleObject(m_stopEvent,kRetryIntervalMs);
            continue;
        }
        // Release exclusive access before pumping again after an activation change.
        UpdateExclusiveFocus(OwnerIsForeground(),std::chrono::steady_clock::now());
        if (m_restartRender.load()) continue;
        if (!DrainCapture()) continue;
        const bool renderReady=wake==WAIT_OBJECT_0+2;
        if (m_exclusiveActive.load()) {
            if (renderReady) m_lastRenderEvent=std::chrono::steady_clock::now();
            else if (std::chrono::steady_clock::now()-m_lastRenderEvent>std::chrono::seconds(2)) {
                HandleStreamError(HRESULT_FROM_WIN32(ERROR_TIMEOUT),L"exclusive render event timeout",false);
                continue;
            }
        }
        if (!FillRender(renderReady)) continue;
        LogStatsIfDue();
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);

    m_streaming = false;

    // Tear down on the thread that built everything, before the apartment goes
    // away, so no interface is released from a foreign apartment.
    if (m_notifier) {
        auto* notifier = static_cast<EndpointNotifier*>(m_notifier.Get());
        if (m_enumerator) m_enumerator->UnregisterEndpointNotificationCallback(notifier);
        notifier->Detach();
        m_notifier.Reset();
    }
    TeardownCapture();
    TeardownRender();
    m_enumerator.Reset();

    CoUninitialize();
}

bool AudioRouter::FifoReset(UINT32 bytesPerFrame, UINT32 samplesPerSec)
{
    m_fifo.clear();
    if (!bytesPerFrame || bytesPerFrame > 256 || !samplesPerSec || samplesPerSec > 768000) return false;
    m_bytesPerFrame = bytesPerFrame;
    m_samplesPerSec = samplesPerSec;
    const size_t frames = (size_t)samplesPerSec * kFifoCapacityMs / 1000;
    if (frames * bytesPerFrame > 64 * 1024 * 1024) return false;
    try { m_fifo.assign(frames * bytesPerFrame, 0); } catch (...) { return false; }
    m_fifoHead   = 0;
    m_fifoBytes  = 0;
    m_fifoPrimed = false;
    ResetDrift();
    m_winStart   = std::chrono::steady_clock::now();
    m_winIn = m_winOut = m_winUnderrun = m_winOverrun = m_winSlipDrop = m_winSlipDup = 0;
    m_winFillMin = 0xFFFFFFFFu;
    m_winFillMax = 0;
    return true;
}

void AudioRouter::FifoPush(const BYTE* data, UINT32 frames, bool silent)
{
    if (m_fifo.empty() || m_bytesPerFrame == 0 || (!silent && !data) || !frames) return;
    size_t bytes = (size_t)frames * m_bytesPerFrame;
    const size_t cap = m_fifo.size();
    if (bytes > cap) {
        m_overrunFrames += (bytes - cap) / m_bytesPerFrame;
        m_winOverrun += (bytes - cap) / m_bytesPerFrame;
        if (!silent) data += (bytes - cap);
        bytes  = cap;
    }
    const size_t free = cap - m_fifoBytes;
    if (bytes > free) {
        // Capture is ahead of playback and the FIFO is full: discard the
        // oldest audio so latency stays bounded. Counted as overrun.
        const size_t drop = bytes - free;
        m_fifoHead   = (m_fifoHead + drop) % cap;
        m_fifoBytes -= drop;
        m_winOverrun    += drop / m_bytesPerFrame;
        m_overrunFrames += drop / m_bytesPerFrame;
    }
    const size_t tail  = (m_fifoHead + m_fifoBytes) % cap;
    const size_t first = (std::min)(bytes, cap - tail);
    if (silent) {
        memset(m_fifo.data() + tail, m_captureFormat && m_captureFormat->wBitsPerSample == 8 ? 128 : 0, first);
        if (bytes > first) memset(m_fifo.data(), m_captureFormat && m_captureFormat->wBitsPerSample == 8 ? 128 : 0, bytes - first);
    } else {
        memcpy(m_fifo.data() + tail, data, first);
        if (bytes > first) memcpy(m_fifo.data(), data + first, bytes - first);
    }
    m_fifoBytes += bytes;
}

UINT32 AudioRouter::FifoPop(BYTE* out, UINT32 frames)
{
    if (!out || !frames || m_fifo.empty() || m_bytesPerFrame == 0) return 0;
    size_t bytes = (std::min)((size_t)frames * m_bytesPerFrame, m_fifoBytes);
    bytes -= bytes % m_bytesPerFrame;
    const size_t cap   = m_fifo.size();
    const size_t first = (std::min)(bytes, cap - m_fifoHead);
    memcpy(out, m_fifo.data() + m_fifoHead, first);
    if (bytes > first) memcpy(out + first, m_fifo.data(), bytes - first);
    m_fifoHead   = (m_fifoHead + bytes) % cap;
    m_fifoBytes -= bytes;
    return (UINT32)(bytes / m_bytesPerFrame);
}

void AudioRouter::FifoSkip(UINT32 frames)
{
    if (m_fifo.empty() || m_bytesPerFrame == 0) return;
    const size_t bytes = (std::min)((size_t)frames * m_bytesPerFrame, m_fifoBytes);
    m_fifoHead   = (m_fifoHead + bytes) % m_fifo.size();
    m_fifoBytes -= bytes;
}

bool AudioRouter::DrainCapture()
{
    UINT32 packetLength = 0;
    HRESULT hr = m_captureService->GetNextPacketSize(&packetLength);
    if (FAILED(hr)) { HandleStreamError(hr, L"GetNextPacketSize", true); return false; }
    while (packetLength != 0 && m_running) {
        BYTE*  data   = nullptr;
        UINT32 frames = 0;
        DWORD  flags  = 0;
        UINT64 devicePosition=0, qpcPosition=0;
        hr = m_captureService->GetBuffer(&data, &frames, &flags, &devicePosition, &qpcPosition);
        if (FAILED(hr)) { HandleStreamError(hr, L"capture GetBuffer", true); return false; }
        if (frames > m_captureBufferFrames || (!data && frames && !(flags & AUDCLNT_BUFFERFLAGS_SILENT))) {
            const HRESULT release = m_captureService->ReleaseBuffer(frames);
            HandleStreamError(FAILED(release) ? release : E_INVALIDARG, L"invalid capture packet", true);
            return false;
        }
        if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) && m_fifoPrimed) {
            m_fifoHead = m_fifoBytes = 0;
            m_fifoPrimed = false;
            ++m_resyncs;
            ResetDrift();
        }
        if (flags & (AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR | AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY))
            m_captureClockRate.Reset();
        else if (frames)
            m_captureClockRate.Sample(devicePosition, qpcPosition, m_samplesPerSec,
                std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
        FifoPush(data, frames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
        m_winIn += frames;
        hr = m_captureService->ReleaseBuffer(frames);
        if (FAILED(hr)) { HandleStreamError(hr, L"capture ReleaseBuffer", true); return false; }
        hr = m_captureService->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) { HandleStreamError(hr, L"GetNextPacketSize", true); return false; }
    }
    return true;
}

void AudioRouter::ResetDrift(bool preserveClock) {
    if (preserveClock) m_drift.Reprime();
    else {
        m_drift.Reset(); m_driftPpm=0;
        m_captureClockRate.Reset(); m_renderClockRate.Reset();
        m_estimatedPpm=0; m_clockEstimateValid=false;
        m_recoveryGuard.ClearWindow();
    }
    m_slipBudget=0;
    m_phase=m_conversionReady ? m_sinc.History() : 1.0;
    m_lastDrift=std::chrono::steady_clock::now();
    m_occupancyStart=m_occupancyLast=m_lastDrift;
    m_occupancyIntegral=m_occupancySeconds=0;
    m_occupancyPrevious=0; m_occupancyMin=UINT32_MAX; m_occupancyMax=0;
}

void AudioRouter::SetupRenderClock() {
    m_renderClock.Reset(); m_renderClockFrequency=0;
    if (FAILED(m_renderClient->GetService(IID_PPV_ARGS(&m_renderClock))) ||
        !m_renderClock || FAILED(m_renderClock->GetFrequency(&m_renderClockFrequency)) ||
        !m_renderClockFrequency) {
        m_renderClock.Reset(); m_renderClockFrequency=0;
    }
}

double AudioRouter::ReadClockEstimate(double now) {
    if (m_renderClock) {
        UINT64 position=0, qpc=0;
        // S_FALSE reports an imprecise measurement; never feed it to the estimator.
        if (m_renderClock->GetPosition(&position,&qpc)==S_OK)
            m_renderClockRate.Sample(position,qpc,double(m_renderClockFrequency),now);
    }
    const bool valid=m_captureClockRate.Valid(now) && m_renderClockRate.Valid(now);
    const double estimate=valid ? (m_captureClockRate.Rate()/m_renderClockRate.Rate()-1)*1e6 : 0;
    m_clockEstimateValid=valid;
    m_estimatedPpm=int(std::lround(estimate));
    return valid ? std::clamp(estimate,-2000.0,2000.0) : 0;
}

void AudioRouter::ObserveRecovery(double now, double error, double missing, double block) {
    if (!m_driftEnabled.load()) { m_recoveryGuard.ClearWindow(); return; }
    if (m_recoveryGuard.Observe(now,error,missing,block)) {
        ++m_automaticReloads;
        // EnsureEndpoints rebuilds both clients on the next worker iteration.
        // No fault latch: the preferred exclusive mode can be opened again.
        m_restartCapture=true;
        AudioLog(L"audio recovery threshold exceeded; reloading endpoints (10 s cooldown, FIFO target unchanged)");
    }
}

UINT32 AudioRouter::FifoResample(BYTE* out, UINT32 frames, double ratio) {
    if (!out || !m_conversionReady || !std::isfinite(ratio) || ratio<=0) return 0;
    const UINT32 fill=UINT32(m_fifoBytes/m_bytesPerFrame);
    const UINT32 history=m_sinc.History(), taps=m_sinc.Taps();
    const UINT32 inBytes=m_inputPcm.bits/8, outBytes=m_outputPcm.bits/8;
    UINT32 written=0;
    for (;written<frames;++written) {
        const UINT32 k=UINT32(m_phase);
        if (k<history || k-history+taps>fill) break;
        const double phase=(m_phase-k)*AudioConvert::SincKernel::Phases;
        const unsigned p=std::min(unsigned(phase),AudioConvert::SincKernel::Phases-1);
        const double fraction=phase-p;
        const float *a=m_sinc.Phase(p), *b=m_sinc.Phase(p+1);
        std::array<double,32> input{}, output{};
        for (unsigned tap=0;tap<taps;++tap) {
            const double weight=a[tap]+fraction*(b[tap]-a[tap]);
            const size_t at=(m_fifoHead+size_t(k-history+tap)*m_bytesPerFrame)%m_fifo.size();
            for (unsigned ch=0;ch<m_inputPcm.channels;++ch)
                input[ch]+=weight*AudioConvert::Read(m_fifo.data()+at+ch*inBytes,m_inputPcm);
        }
        AudioConvert::Map(input.data(),m_inputPcm,output.data(),m_outputPcm);
        for (unsigned ch=0;ch<m_outputPcm.channels;++ch)
            AudioConvert::Write(out+size_t(written)*m_renderFormat->nBlockAlign+ch*outBytes,m_outputPcm,output[ch]);
        m_phase+=ratio;
    }
    // Retain FIR history plus the fractional phase across arbitrary packets.
    const UINT32 consumed=UINT32(m_phase)-history;
    FifoSkip(consumed); m_phase-=consumed;
    return written;
}

void AudioRouter::PublishQueue(UINT32 padding) {
    const UINT32 history=m_conversionReady ? m_sinc.History() : 0;
    const UINT32 fill=UINT32(m_fifoBytes/m_bytesPerFrame);
    const UINT32 fifoUs=UINT32(uint64_t(fill>history ? fill-history : 0)*1000000/m_samplesPerSec);
    const UINT32 queueUs=UINT32(uint64_t(padding)*1000000/m_renderFormat->nSamplesPerSec);
    m_queueSnapshot=(uint64_t(fifoUs)<<32)|queueUs;
    m_fillMs=fifoUs/1000; m_queueMs=queueUs/1000;
    const auto now=std::chrono::steady_clock::now();
    const double dt=std::chrono::duration<double>(now-m_occupancyLast).count();
    m_occupancyLast=now;
    m_occupancyIntegral+=m_occupancyPrevious*dt; m_occupancySeconds+=dt;
    m_occupancyPrevious=fifoUs;
    m_occupancyMin=std::min(m_occupancyMin,fifoUs); m_occupancyMax=std::max(m_occupancyMax,fifoUs);
    if (now-m_occupancyStart>=std::chrono::seconds(1)) {
        m_fifoAverageUs=m_occupancySeconds>0 ? UINT32(m_occupancyIntegral/m_occupancySeconds) : fifoUs;
        m_fifoMinUs=m_occupancyMin; m_fifoMaxUs=m_occupancyMax;
        m_occupancyStart=now; m_occupancyIntegral=m_occupancySeconds=0;
        m_occupancyMin=UINT32_MAX; m_occupancyMax=0;
    }
}

bool AudioRouter::FillRender(bool renderReady)
{
    if (!m_bytesPerFrame || !m_samplesPerSec || !m_renderFormat) return true;
    const bool exclusive=m_exclusiveActive.load();
    // Capture notifications/timeouts must never write an exclusive ping-pong buffer.
    if (exclusive && !renderReady) return true;
    UINT32 padding=0;
    HRESULT hr=S_OK;
    if (!exclusive) {
        hr=m_renderClient->GetCurrentPadding(&padding);
        if (FAILED(hr)) { HandleStreamError(hr,L"GetCurrentPadding",false); return false; }
    }
    const UINT32 renderRate=m_renderFormat->nSamplesPerSec;
    const double baseRatio=double(m_samplesPerSec)/renderRate;
    const auto sourceFrames=[this](int ms) { return UINT32(uint64_t(m_samplesPerSec)*ms/1000); };
    const auto renderFrames=[renderRate](int ms) { return UINT32(uint64_t(renderRate)*ms/1000); };
    const UINT32 queue=exclusive ? m_renderBufferFrames : std::min(m_renderBufferFrames,std::max(renderFrames(m_renderQueueTargetMs.load()),
        m_renderPeriodFrames+renderFrames(1)));
    m_effectiveQueueUs=UINT32(uint64_t(queue)*1000000/renderRate);
    if (padding>=queue) { PublishQueue(padding); return true; }
    const UINT32 want=queue-padding;
    const auto pumpNow=std::chrono::steady_clock::now();
    const double nowSeconds=std::chrono::duration<double>(pumpNow.time_since_epoch()).count();
    const double clockPpm=ReadClockEstimate(nowSeconds);
    UINT32 fill=UINT32(m_fifoBytes/m_bytesPerFrame);
    const UINT32 history=m_conversionReady ? m_sinc.History() : 0;
    const UINT32 lookahead=m_conversionReady ? m_sinc.Taps()-history : 0;
    // All FIFO/controller math is in SOURCE frames. Padding and requests are
    // in RENDER frames and must be converted when nominal rates differ.
    const UINT32 need=UINT32(std::ceil(want*baseRatio*1.002))+lookahead+1;
    const UINT32 reserve=std::max(sourceFrames(m_fifoTargetMs.load()),lookahead+1);
    const double target=reserve+queue*baseRatio;
    const double error=(double(fill)-history+padding*baseRatio-target)/m_samplesPerSec;
    if (double(fill)+padding*baseRatio>history+target+sourceFrames(std::max(20,m_fifoTargetMs.load()))) {
        const UINT32 keep=history+reserve+need;
        if (fill>keep) {
            const UINT32 drop=fill-keep; FifoSkip(drop); fill-=drop;
            m_overrunFrames+=drop; m_winOverrun+=drop; ++m_resyncs; ResetDrift(true);
        }
    }
    if (!m_fifoPrimed) {
        if (fill<history+reserve+need) {
            if (exclusive) {
                BYTE* silent=nullptr;
                hr=m_renderService->GetBuffer(want,&silent);
                if (SUCCEEDED(hr)) hr=m_renderService->ReleaseBuffer(want,AUDCLNT_BUFFERFLAGS_SILENT);
                if (FAILED(hr)) { HandleStreamError(hr,L"exclusive priming silence",false); return false; }
            }
            PublishQueue(exclusive ? queue : padding); return true;
        }
        m_fifoPrimed=true; ResetDrift(true);
    }
    const auto now=std::chrono::steady_clock::now();
    const double dt=std::chrono::duration<double>(now-m_lastDrift).count(); m_lastDrift=now;
    // Count only samples available to the converter, not its FIR history or
    // future taps. Observe the trough across packets before speeding up input.
    const double headroom=(double(fill)-(m_conversionReady ? m_phase+lookahead : 0)
        -want*baseRatio)/m_samplesPerSec;
    const double ppm=m_driftEnabled.load() ? m_drift.Update(
        (double(fill)-history+padding*baseRatio-target)/m_samplesPerSec,dt,clockPpm,headroom) : 0;
    m_driftPpm=int(std::lround(ppm));
    BYTE* out=nullptr;
    hr=m_renderService->GetBuffer(want,&out);
    if (FAILED(hr)) { HandleStreamError(hr,L"render GetBuffer",false); return false; }
    UINT32 written=0;
    if (m_conversionReady) {
        // Nominal format conversion stays active even when drift is disabled.
        written=FifoResample(out,want,baseRatio*(1+ppm/1000000));
    } else {
        // Only the legacy path reaches here: identical capture/render format.
        m_slipBudget+=want*ppm/1000000;
        const UINT32 drop=m_slipBudget>=1 ? std::min(UINT32(m_slipBudget),fill) : 0;
        if (drop) { FifoSkip(drop); fill-=drop; m_slipBudget-=drop; m_slipCount+=drop; m_winSlipDrop+=drop; }
        const UINT32 dup=m_slipBudget<=-1 && want>1 && fill ? std::min(UINT32(-m_slipBudget),want-1) : 0;
        written=FifoPop(out,std::min(want-dup,fill));
        if (written && dup) {
            for (UINT32 i=0;i<dup;++i) memcpy(out+size_t(written+i)*m_bytesPerFrame,
                out+size_t(written-1)*m_bytesPerFrame,m_bytesPerFrame);
            written+=dup; m_slipBudget+=dup; m_slipCount+=dup; m_winSlipDup+=dup;
        }
    }
    if (written<want) {
        memset(out+size_t(written)*m_renderFormat->nBlockAlign,
            m_renderFormat->wBitsPerSample==8 ? 128 : 0,size_t(want-written)*m_renderFormat->nBlockAlign);
        m_underrunFrames+=want-written; m_winUnderrun+=want-written;
        ++m_underrunEvents;
        m_fifoPrimed=false; ResetDrift(true);
    }
    const bool silent=m_muted.load() || written==0;
    const float volume=m_volume.load();
    if (!silent && volume<0.999f) ScaleInPlace(out,want,m_renderFormat,volume);
    hr=m_renderService->ReleaseBuffer(want,silent ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
    if (FAILED(hr)) { HandleStreamError(hr,L"render ReleaseBuffer",false); return false; }
    ObserveRecovery(nowSeconds,error,double(want-written)/renderRate,double(want)/renderRate);
    m_winOut+=want;
    // Refresh actual padding after release, rather than reporting the target.
    if (!exclusive) {
        hr=m_renderClient->GetCurrentPadding(&padding);
        if (FAILED(hr)) { HandleStreamError(hr,L"post-render padding",false); return false; }
    }
    // In exclusive mode this is the submitted block size, NOT measured padding.
    PublishQueue(exclusive ? want : padding);
    m_winFillMin=std::min(m_winFillMin,m_fillMs.load()); m_winFillMax=std::max(m_winFillMax,m_fillMs.load());
    return true;
}

static bool IsAudioWindowForeground(HWND owner, HWND foreground) {
    if (!owner || !IsWindow(owner) || !IsWindowVisible(owner) || IsIconic(owner)) return false;
    // Includes hosted WebView controls and owned settings/dialog windows.
    return foreground && (foreground==owner || IsChild(owner,foreground) ||
                           GetAncestor(foreground,GA_ROOTOWNER)==owner);
}

bool AudioRouter::OwnerIsForeground() const {
    return IsAudioWindowForeground(m_ownerWindow.load(),GetForegroundWindow());
}

void AudioRouter::UpdateExclusiveFocus(bool foreground, ExclusiveFocusGate::Clock::time_point now) {
    const bool allowed=m_focusGate.Update(foreground,now);
    if (m_exclusivePermitted.exchange(allowed)==allowed || !m_exclusiveRequested.load()) return;
    if (allowed) m_retryExclusive=true; // a new foreground session may retry a previous fallback
    m_restartRender=true;
}

void AudioRouter::SetExclusive(bool enabled) {
    if (m_exclusiveRequested.exchange(enabled)!=enabled) {
        m_retryExclusive=true;
        m_restartRender=true;
    }
}

void AudioRouter::SetLatency(int fifoMs, int renderMs, bool drift) {
    fifoMs = std::clamp(fifoMs, 3, 100);
    renderMs = std::clamp(renderMs, 3, 100);
    const bool fifoChanged = m_fifoTargetMs.exchange(fifoMs) != fifoMs;
    const bool queueChanged = m_renderQueueTargetMs.exchange(renderMs) != renderMs;
    const bool driftChanged = m_driftEnabled.exchange(drift) != drift;
    if (fifoChanged || queueChanged || driftChanged) m_restartRender = true;
}

void AudioRouter::LogStatsIfDue()
{
    const auto now = std::chrono::steady_clock::now();
    if (now - m_winStart < std::chrono::milliseconds(kStatsIntervalMs)) return;
    std::wstringstream ss;
    ss << L"stats: fill " << m_fillMs.load() << L" ms (min "
       << (m_winFillMin == 0xFFFFFFFFu ? 0u : m_winFillMin) << L", max " << m_winFillMax
       << L"), in " << m_winIn << L", out " << m_winOut
       << L", underrun " << m_winUnderrun << L", overrun " << m_winOverrun
       << L", render " << m_queueMs.load() << L" ms, drift " << m_driftPpm.load()
       << L" ppm, estimate " << m_estimatedPpm.load() << L" ppm (valid " << m_clockEstimateValid.load()
       << L"), underrun events " << m_underrunEvents.load() << L", automatic reloads " << m_automaticReloads.load()
       << L", resync " << m_resyncs.load()
       << L", slip +" << m_winSlipDrop << L"/-" << m_winSlipDup;
    AudioLog(ss.str());
    m_winStart = now;
    m_winIn = m_winOut = m_winUnderrun = m_winOverrun = m_winSlipDrop = m_winSlipDup = 0;
    m_winFillMin = 0xFFFFFFFFu;
    m_winFillMax = 0;
}

void AudioRouter::SetVolume(float volume)
{
    if (std::isfinite(volume)) m_volume = std::clamp(volume, 0.0f, 1.0f);
}

void AudioRouter::SetMuted(bool muted)
{
    m_muted = muted;
}

} // namespace NitLink
