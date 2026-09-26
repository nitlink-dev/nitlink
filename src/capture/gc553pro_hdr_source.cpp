#include "gc553pro_hdr_source.h"

#include <windows.h>
#include <dshow.h>
#include <ks.h>
#include <atlbase.h>
#include <atlcomcli.h>
#include <array>
#include <vector>
#include <cwctype>

namespace NitLink {

Gc553ProHdrProbe DecodeGc553ProHdrResponse(const uint8_t* data,
                                              size_t size) noexcept {
    // Four live A/B samples all returned 37 bytes. The final four bytes
    // vary with EOTF and are opaque; do not mistake them for the payload.
    if (!data || size != 37 || data[0] != 0xA1 || data[1] != 0x22 ||
        data[2] != 0 || data[3] != 0 || data[4] != 0x87 ||
        data[5] != 0x01 || data[6] != 0x1A ||
        data[8] != 0x00) return {};

    switch (data[7]) {
    case 0x00: return {Gc553ProSourceHdrState::Sdr, data[7]};
    case 0x02: return {Gc553ProSourceHdrState::Hdr10Pq, data[7]};
    case 0x01:
    case 0x03: return {Gc553ProSourceHdrState::OtherHdr, data[7]};
    default: return {};
    }
}

namespace {
std::wstring Lower(std::wstring value) {
    for (auto& c : value) c = static_cast<wchar_t>(std::towlower(c));
    return value;
}

bool IsSelectedGc553Pro(const std::wstring& friendlyName,
                         const std::wstring& requestedName) {
    const auto candidate = Lower(friendlyName);
    const auto requested = Lower(requestedName);
    const bool sameName = candidate == requested ||
        candidate.find(requested) != std::wstring::npos ||
        requested.find(candidate) != std::wstring::npos;
    const bool gcIdentity = candidate.find(L"gc553pro") != std::wstring::npos ||
        candidate.find(L"live gamer ultra s") != std::wstring::npos;
    const bool selectedGcIdentity = requested.find(L"gc553pro") != std::wstring::npos ||
        requested.find(L"live gamer ultra s") != std::wstring::npos;
    // MF and DirectShow can publish different aliases for the same model.
    return gcIdentity && (sameName || selectedGcIdentity);
}

constexpr GUID kXuSet = {0x961073C7,0x49F7,0x44F2,{0xAB,0x42,0xE9,0x40,0x40,0x59,0x40,0xC2}};
constexpr GUID kDevSpecific = {0x941C7AC0,0xC559,0x11D0,{0x8A,0x2B,0x00,0xA0,0xC9,0x25,0x5A,0xC1}};
constexpr GUID kTopologyIid = {0x720D4AC0,0x7533,0x11D0,{0xA5,0xD6,0x28,0xDB,0x04,0xC1,0x00,0x00}};
constexpr GUID kControlIid = {0x28F54685,0x06FD,0x11D2,{0xB2,0x7A,0x00,0xA0,0xC9,0x22,0x31,0x96}};

struct IKsTopologyInfoLocal : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_NumCategories(DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Category(DWORD, GUID*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_NumConnections(DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ConnectionInfo(DWORD, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_NodeName(DWORD, WCHAR*, DWORD, DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_NumNodes(DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_NodeType(DWORD, GUID*) = 0;
};
struct IKsControlLocal : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE KsProperty(PKSPROPERTY, ULONG, void*, ULONG, ULONG*) = 0;
};

bool Mailbox(IKsControlLocal* ctrl, ULONG node,
             const std::array<uint8_t, 9>& request,
             std::vector<uint8_t>& response) {
    auto property = [&](ULONG selector, ULONG flag, void* buffer,
                        ULONG size, ULONG* returned) {
        KSP_NODE p{};
        p.Property.Set = kXuSet;
        p.Property.Id = selector;
        p.Property.Flags = flag | KSPROPERTY_TYPE_TOPOLOGY;
        p.NodeId = node;
        return ctrl->KsProperty(reinterpret_cast<PKSPROPERTY>(&p),
                                sizeof(p), buffer, size, returned);
    };
    USHORT requestSize = static_cast<USHORT>(request.size());
    ULONG returned = 0;
    if (FAILED(property(2, KSPROPERTY_TYPE_SET, &requestSize,
                        sizeof(requestSize), &returned))) return false;
    Sleep(3);
    if (FAILED(property(1, KSPROPERTY_TYPE_SET,
                        const_cast<uint8_t*>(request.data()), request.size(),
                        &returned))) return false;
    Sleep(3);

    USHORT length = 0;
    bool gotHeader = false;
    for (int i = 0; i < 16; ++i) {
        uint8_t header[2]{};
        returned = 0;
        if (SUCCEEDED(property(2, KSPROPERTY_TYPE_GET, header, sizeof(header),
                               &returned)) && returned == sizeof(header)) {
            length = static_cast<USHORT>(header[0] | (header[1] << 8));
            if (length) { gotHeader = true; break; }
        }
        Sleep(4);
    }
    if (!gotHeader || length > 256) return false;
    response.assign(length, 0);
    returned = 0;
    if (FAILED(property(1, KSPROPERTY_TYPE_GET, response.data(), length,
                        &returned)) || returned != length) return false;
    return true;
}
} // namespace

struct Gc553ProHdrReader::Impl {
    bool comInitialized = false;
    CComPtr<IBaseFilter> filter;
    CComPtr<IKsControlLocal> control;
    ULONG node = 0;
    bool ready = false;

    explicit Impl(const std::wstring& deviceName) {
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(init)) return;
        // Both S_OK and S_FALSE require a matching CoUninitialize.
        comInitialized = true;
        CComPtr<ICreateDevEnum> enumerator;
        if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator)))) return;
        CComPtr<IEnumMoniker> devices;
        if (enumerator->CreateClassEnumerator(CLSID_VideoInputDeviceCategory,
                &devices, 0) != S_OK || !devices) return;
        CComPtr<IMoniker> moniker;
        ULONG fetched = 0;
        while (devices->Next(1, &moniker, &fetched) == S_OK) {
            CComPtr<IPropertyBag> bag;
            if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr,
                                                 IID_PPV_ARGS(&bag)))) {
                CComVariant name;
                if (SUCCEEDED(bag->Read(L"FriendlyName", &name, nullptr)) &&
                    name.vt == VT_BSTR && name.bstrVal &&
                    IsSelectedGc553Pro(name.bstrVal, deviceName) &&
                    SUCCEEDED(moniker->BindToObject(nullptr, nullptr,
                                                    IID_PPV_ARGS(&filter)))) break;
            }
            moniker.Release();
        }
        if (!filter) return;
        CComPtr<IKsTopologyInfoLocal> topology;
        if (FAILED(filter->QueryInterface(kTopologyIid,
                    reinterpret_cast<void**>(&topology))) || !topology ||
            FAILED(filter->QueryInterface(kControlIid,
                    reinterpret_cast<void**>(&control))) || !control) return;
        DWORD count = 0;
        if (FAILED(topology->get_NumNodes(&count))) return;
        for (DWORD i = 0; i < count; ++i) {
            GUID type{};
            if (SUCCEEDED(topology->get_NodeType(i, &type)) &&
                IsEqualGUID(type, kDevSpecific)) {
                node = i;
                ready = true;
                break;
            }
        }
    }

    ~Impl() {
        control.Release();
        filter.Release();
        if (comInitialized) CoUninitialize();
    }
};

Gc553ProHdrReader::Gc553ProHdrReader(const std::wstring& deviceName)
    : m_impl(std::make_unique<Impl>(deviceName)) {}
Gc553ProHdrReader::~Gc553ProHdrReader() = default;

Gc553ProHdrProbe Gc553ProHdrReader::Read() {
    if (!m_impl || !m_impl->ready) {
        return {};
    }
    constexpr std::array<uint8_t, 9> readyRequest{
        0xA0,0x06,0x00,0x00,0x67,0x00,0x00,0x00,0xF3};
    constexpr std::array<uint8_t, 9> hdrRequest{
        0xA1,0x06,0x00,0x00,0x65,0x00,0x00,0x00,0xF4};
    std::vector<uint8_t> response;
    if (!Mailbox(m_impl->control, m_impl->node, readyRequest, response)) {
        return {};
    }
    if (response.size() != 9 || response[0] != 0xA0 ||
        response[1] != 0x06 || response[2] != 0 ||
        response[3] != 0 || response[4] != 0x03) {
        return {};
    }
    if (!Mailbox(m_impl->control, m_impl->node, hdrRequest, response)) {
        return {};
    }
    const auto decoded = DecodeGc553ProHdrResponse(response.data(), response.size());
    return decoded;
}

} // namespace NitLink
