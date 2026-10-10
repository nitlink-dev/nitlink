#pragma once

// Optional DirectShow capture backend, selected by a USE_DSHOW.txt marker next
// to the exe. Uses a one-hop push path: the device capture pin connects
// DIRECTLY to a hand-rolled sink filter whose input pin fires a callback the
// instant a frame arrives, with no Media Foundation source-reader queue in
// between. CaptureDevice delegates to this when the marker is present; the
// Media Foundation path is otherwise unchanged.

#include "capture_device.h"   // CaptureFormat, DeviceInfo, FrameCallback

#include <memory>
#include <atomic>
#include <string>

namespace NitLink {

// Opaque graph/filter state, fully defined in the .cpp so the DirectShow COM
// machinery stays out of the header.
struct DShowGraph;

class DShowCapture {
public:
    DShowCapture();
    ~DShowCapture();

    // Build the graph (device -> sink) and negotiate NV12 at the native best
    // resolution/fps. Does NOT start streaming yet (StartCapture does).
    bool Open(const DeviceInfo& device);
    void Close();

    // Install the frame callback and Run() the graph. Frames are delivered on
    // the DirectShow streaming thread, same threading contract as the Media
    // Foundation worker.
    bool StartCapture(FrameCallback callback);
    void StopCapture();

    CaptureFormat GetOutputFormat() const { return m_format; }
    std::wstring   GetDeviceName()   const { return m_deviceName; }
    bool           IsCapturing()     const { return m_capturing.load(); }
    bool           ConsumeNeedsReopen() { return m_needsReopen.exchange(false); }

private:
    friend struct CaptureDeviceFormatTests;
    void ReadFrameRateFromInterval(int64_t interval);
    std::unique_ptr<DShowGraph> m_impl;
    std::wstring       m_deviceName;
    CaptureFormat      m_format;      // written once in Open, read by render thread
    std::atomic<bool>  m_capturing{false};
    std::atomic<bool>  m_needsReopen{false};
};

} // namespace NitLink
