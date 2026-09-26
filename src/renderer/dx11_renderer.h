#pragma once

#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <chrono>

using Microsoft::WRL::ComPtr;

namespace NitLink {

class DX11Renderer {
public:
    DX11Renderer();
    ~DX11Renderer();

    bool Initialize(HWND hwnd, uint32_t width, uint32_t height);
    void Resize(uint32_t width, uint32_t height);

    // doWait=true (default): BeginFrame blocks on the DXGI frame-latency
    // waitable at its start -- the VRR/"Smooth" path. doWait=false: the caller
    // already waited via WaitForFrameReady() at the top of the loop (the
    // Low-Latency present-on-arrival path), so BeginFrame must not wait again.
    void BeginFrame(bool doWait = true);
    // Block on the frame-latency waitable WITHOUT starting the frame, so the
    // Low-Latency loop can wait first, THEN read the freshest capture frame.
    void WaitForFrameReady();
    void DrawCaptureFrame();
    void EndFrame();

    // Latched true after EndFrame's Present (or a per-frame capture-texture
    // Map) reports the graphics device was removed or reset (TDR, driver
    // upgrade, GPU reset). The run loop polls this once per iteration and,
    // when set, rebuilds the renderer and its device-dependent objects.
    // Returns the latched value and clears it, so one device-loss event
    // drives exactly one rebuild. Mirrors the capture side's needs-reopen
    // signal.
    bool ConsumeDeviceLost();

    void UpdateCaptureTexture(const uint8_t* data, uint32_t size, uint32_t width, uint32_t height);

    // Invalidate presentation state from the previous capture session without
    // destroying the renderer resources. The next accepted frame restores it.
    void InvalidateCaptureFrame() { m_hasFrame = false; }
    bool HasCaptureFrame() const { return m_hasFrame; }

    bool SaveScreenshot(const std::wstring& path);

    // Runtime pipeline knobs (wired up by the settings panel).
    // Color expansion smoothly lerps the limited->full transform on/off.
    void SetColorExpansion(bool enabled) {
        m_colorExpansionTarget = enabled ? 1.0f : 0.0f;
        if (!m_colorExpansionInitialized) {
            m_colorExpansionCurrent = m_colorExpansionTarget;
            m_colorExpansionInitialized = true;
        }
    }

    // -- HDR ---------------------------------------------------------------
    // Toggle the swap chain between SDR (B8G8R8A8 + sRGB) and HDR10
    // (R10G10B10A2 + DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020: PQ in
    // BT.2020 primaries). Recreates the swap chain backbuffer; caller must
    // release any D2D bitmaps wrapping it first (see overlay/OnResizeBegin/
    // OnResizeEnd flow).
    //
    // Returns true on success. On failure (display doesn't support HDR,
    // Windows HDR not engaged at the OS level, colorspace not supported on
    // the R10G10B10A2 chain, etc) the renderer stays in SDR mode and
    // returns false; the settings UI should reflect this.
    bool SetHDREnabled(bool enable);
    bool IsHDREnabled() const { return m_hdrEnabled; }

    // Composite a pre-rendered BGRA8 UI texture onto the current backbuffer.
    // Used in HDR mode where D2D can't draw to the R10G10B10A2 HDR10 PQ
    // backbuffer directly: the Overlay class renders into a BGRA8 offscreen
    // instead, and this method samples that offscreen as a fullscreen-blit
    // textured quad. The UI pixel shader does sRGB-to-linear (gamma 2.2
    // approximation, not piecewise), then x203 nits, then PQ-encode, matching
    // Windows' SDR-in-HDR paper-white reference. No BT.709-to-BT.2020 primary
    // rotation: UI is largely neutral so the residual hue mismatch is
    // negligible.
    //
    // HDR-only path. In SDR mode Overlay::IsUsingOffscreen() returns false
    // and the caller skips this composite; the shader unconditionally
    // writes PQ values, which would render wrong on an sRGB backbuffer.
    void CompositeUI(ID3D11ShaderResourceView* uiSRV);

    // True iff the connected display reports HDR10 capability. Independent
    // of whether Windows HDR mode is actually engaged.
    bool IsHDRDisplaySupported() const { return m_hdrDisplaySupported; }
    // True iff Windows is in HDR mode for the relevant output right now.
    // When false, setting HDR on the swap chain still works but DWM will
    // tone-map the output back to SDR for display.
    bool IsHDREngagedByOS() const { return m_hdrEngagedByOS; }
    // Re-query the display's HDR state. Cheap; safe to call per-frame from
    // status UI so toggling Windows HDR mode reflects immediately.
    void RefreshHDRDisplayInfo() { QueryHDRDisplayInfo(); }

    // Display luminance range from the connected display, populated by the
    // last call to QueryHDRDisplayInfo(). Used by the test pattern shader
    // to scale brightness ladders into the display's actual capability.
    float GetDisplayMinLuminance() const { return m_displayMinLum; }
    float GetDisplayMaxLuminance() const { return m_displayMaxLum; }

    // Development-time HDR test pattern. Draws a brightness ladder of
    // SDR-white and HDR-luminance squares into the HDR backbuffer instead
    // of the capture frame, to confirm the swap-chain colour space is
    // composing PQ values correctly. Not wired to any user hotkey in the
    // rc1 build; the real HDR capture path (P010 + HDR10 PQ swap chain)
    // is the shipping configuration. Kept for in-shop diagnostics.
    void DrawHDRTestPattern();

    // Diagnostic overlay for HDR color-fidelity debugging. Draws calibration
    // patches over the bottom of the captured frame with KNOWN scRGB values
    // (pure white at SDR ref, mid-grey, reference orange, reference red,
    // reference skin tone). Enables A/B against a direct-to-TV HDR signal:
    // if the patches look identical to the same patches rendered natively,
    // the pipeline is correct. If they shift, the bias is measurable.
    // Toggled by Ctrl+F4 in application.cpp.
    void DrawHDRDiagnostics();
    bool IsHDRDiagModeOn() const { return m_hdrDiagMode; }
    void SetHDRDiagMode(bool on) { m_hdrDiagMode = on; }

    // VSync present mode. When on, EndFrame presents with sync (Present(1, 0))
    // instead of the default immediate ALLOW_TEARING present, letting a VRR or
    // fixed-refresh display handle tearing. Trades a little latency for none.
    bool IsVSyncOn() const { return m_vsync; }
    void SetVSync(bool on) { m_vsync = on; }
    bool m_vsync = false;

    // Present-rate cap for the tearing-allowed present, in Hz; 0 turns it
    // off. Chosen by the application from the display refresh rate, the
    // source frame rate, and the present_cap_hz config key. Returns false
    // and leaves the cap alone when a VRR_CAP.txt marker pinned it at
    // Initialize: the marker is a hand-set test rate and must not be
    // replaced by policy.
    bool SetPresentCap(double hz);

    // Average milliseconds per phase since the previous call, then resets.
    // waitMs and uploadMs are per iteration (WaitForFrameReady counts the
    // iterations), presentMs is per presented frame.
    struct PhaseTimes { double waitMs; double uploadMs; double presentMs; uint32_t iterations; uint32_t presents; };
    PhaseTimes ConsumePhaseTimes();

    // How the swap chain reached the screen on the last frame, as reported by
    // DXGI (DXGI_FRAME_PRESENTATION_MODE: 0 composed by the desktop
    // compositor, 1 hardware overlay, 2 none, 3 composition failure), or -1
    // when the query is unavailable. Composed presentation costs a full
    // compositor pass per present and adds a frame of latency.
    int PresentationMode() const;

    // Display aspect override. 0 shows the source at its own ratio, a
    // positive value forces that width-to-height ratio (4:3 squeezes a
    // stretched retro source back into shape), and a negative value fills
    // the window with no letterboxing at all.
    void  SetAspectOverride(float ratio) { m_aspectOverride = ratio; }
    float GetAspectOverride() const { return m_aspectOverride; }

    ID3D11Device*        GetDevice()    const { return m_device.Get(); }
    ID3D11DeviceContext* GetContext()   const { return m_context.Get(); }
    IDXGISwapChain1*     GetSwapChain() const { return m_swapChain.Get(); }

    // Returns the most recent averaged end-to-end render time in milliseconds.
    // Averaged over a sliding window inside the renderer; the value updates
    // a few times per second. Used by the settings UI to show a live latency
    // readout.
    double                GetAverageRenderMs() const { return m_lastReportedRenderMs; }

    // Returns the most recent measured GPU per-frame work in milliseconds,
    // sourced from D3D11_QUERY_TIMESTAMP bracketing the per-frame draws.
    // Lags by ~3 frames because the readback uses a ring buffer to avoid
    // forcing a GPU stall on GetData. Returns 0 until enough frames have
    // elapsed for the first readback, or if query creation failed.
    double                GetLastGpuMs() const { return m_lastGpuMs; }

    // The raw captured-frame SRV. For BGRA captures this is the full color
    // texture; for NV12 it's the Y plane (R8). Either way `.r` gives luma:
    // adequate for new-frame detection (FrameDiffer).
    ID3D11ShaderResourceView* GetRawCaptureSRV() const { return m_captureSRV.Get(); }

    // Tell the renderer whether the active capture source delivers its rows
    // top-down (modern Media Foundation default; planar formats like P010
    // and NV12 are always top-down; Elgato 4K S USB) or bottom-up (legacy
    // GDI convention; some older drivers; Elgato 4K Pro PCIe in RGB32).
    // The shared vertex buffer pre-flips V (legacy assumption), so when the
    // source is genuinely top-down the BGRA shader needs to flip V *back*.
    // This call sets a flag the BGRA shader reads from its constant buffer
    // to decide. P010 and overlay paths already have their own internal
    // flips and are unaffected.
    void SetSourceRowOrder(bool topDown) { m_sourceTopDown = topDown; }

    // Tell the renderer whether the active capture source delivers pixels
    // in FULL range (0-255 / 0..1) or LIMITED range (16-235 / TV range).
    // Driven by CaptureFormat::fullRange which comes from the driver's
    // MF_MT_VIDEO_NOMINAL_RANGE attribute. Most HDMI sources are limited
    // range, but some Elgato NV12 paths report full range because the
    // driver converted YUV to full-range before handing the frames over.
    // The BGRA and NV12 shaders read this to skip the limited-to-full
    // expansion when not needed (preventing crushed blacks).
    void SetSourceFullRange(bool fullRange) { m_sourceFullRange = fullRange; }

    // Model-specific P010 chroma interpretation. Default false preserves the
    // existing capture-card behavior; only effective limited mode consumes it.
    void SetP010LimitedChroma(bool enabled) { m_p010LimitedChroma = enabled; }

    // Tell the renderer whether the active capture source is known HDR10.
    // GC553Pro Auto uses XU/EOTF; existing device paths retain their current
    // negotiated-format policy. The capture format is set separately by
    // SetSourceFormat, and the output preference by SetHDREnabled.
    //
    // The P010 shader currently uses this to select its HDR-to-SDR tone map:
    //   sourceIsHDR10 && !hdrEnabled -> PQ decode -> linear -> BT.2020 to
    //                                  BT.709 -> BT.2446A-derived luminance
    //                                  EETF -> sRGB
    //                                  to SDR backbuffer (in-shader HDR-to-SDR
    //                                  path)
    // Its other P010 branch still expects PQ/BT.2020 input. SDR pixels
    // delivered in P010 by GC553Pro have not yet been characterized; this
    // flag does not assert their transfer function or make that branch SDR-safe.
    //
    // The second case exists because the Elgato hardware tonemap toggle
    // doesn't reliably re-engage mid-session via IKsPropertySet: the card
    // seems to latch its tonemap state at HDMI signal acquisition. Doing
    // the tonemap in the shader bypasses that firmware quirk entirely and
    // gives full control over the luminance-preserving operator.
    void SetSourceIsHDR10(bool isHDR10) { m_sourceIsHDR10 = isHDR10; }

    // The pixel formats this renderer's capture path knows how to handle.
    // Exposed publicly so the application layer can map the Media Foundation
    // subtype GUID it negotiated with CaptureDevice into a stable enum that
    // doesn't drag <mfapi.h> into every header that wants to talk format.
    enum class CaptureFormatKind {
        BGRA,   // RGB32/BGRA: SDR sRGB-ish bytes (default)
        NV12,   // 8-bit YUV 4:2:0: used when MF delivers NV12
        P010,   // 10-bit BT.2020 PQ: the real HDR10 path
    };

    // Declare which pixel format the capture pipeline is currently producing.
    // Called by the application after CaptureDevice::Open() succeeds (both at
    // Initialize and at every ReconcileCaptureFormat). This REPLACES the old
    // per-frame byte-count inference inside UpdateCaptureTexture, which could
    // mis-classify partial frames produced during HDMI signal-loss windows
    // and upload them as the wrong format: the all-green frame previously
    // seen during PS5 SDR<->HDR transitions.
    //
    // Must be called BEFORE the first UpdateCaptureTexture call after a
    // format change. The application currently sequences this correctly:
    // SetSourceFormat runs inside the Initialize / Reconcile pre-StartCapture
    // window, so the capture worker thread doesn't exist yet when the format
    // is declared and there's no race.
    //
    // GPU resources are recreated lazily on the next UpdateCaptureTexture
    // call when the declared format differs from the currently-allocated
    // resources format.
    void SetSourceFormat(CaptureFormatKind kind) {
        m_sourceFormat    = kind;
        m_sourceFormatSet = true;
    }

    // -- Upscaler integration ---------------------------------------------
    // DrawCaptureFrame writes the Catmull-Rom + range-expanded result to a
    // post-input intermediate (sized to capture resolution) whenever upscaling
    // is enabled. An upscaler reads m_postInputSRV, runs its compute pass, and
    // hands its output SRV back to CompositeUpscaledTexture, which blits to
    // the backbuffer with aspect-correct letterboxing.
    void CompositeUpscaledTexture(ID3D11ShaderResourceView* upscaledSRV,
                                    uint32_t srcW, uint32_t srcH);

    // The intermediate target that DrawCaptureFrame uses when upscaling is
    // engaged. Width/Height match the current capture frame size, NOT the
    // window, so NIS receives a clean source at native resolution.
    ID3D11ShaderResourceView* GetCaptureOutputSRV() const { return m_postInputSRV.Get(); }
    uint32_t GetCaptureOutputWidth()  const { return m_postInputW; }
    uint32_t GetCaptureOutputHeight() const { return m_postInputH; }
    uint32_t GetWindowWidth()  const { return m_windowWidth; }
    uint32_t GetWindowHeight() const { return m_windowHeight; }

    // Enable/disable the "render through post-input intermediate" path.
    // Both NIS upscaling AND frame generation need the renderer to produce
    // a clean RGBA8 capture-resolution intermediate texture rather than
    // drawing straight to the backbuffer. When OFF, DrawCaptureFrame goes
    // directly to backbuffer like before for minimum latency.
    void SetPostInputEnabled(bool on) { m_postInputEnabled = on; }
    bool IsPostInputEnabled() const { return m_postInputEnabled; }
    // Back-compat alias used during the NIS-only era.
    void SetUpscalingEnabled(bool on) { SetPostInputEnabled(on); }

private:
    bool CreateRenderTarget();
    void ReleaseRenderTarget();
    bool CreatePostInputTarget(uint32_t w, uint32_t h);
    void ReleasePostInputTarget();
    bool CreateCompositeShader();
    bool CreateCaptureResources(uint32_t width, uint32_t height, bool isNV12);
    bool CreateCaptureResourcesP010(uint32_t width, uint32_t height);
    bool CreateFullscreenQuad();
    bool CreateGpuTimingQueries();
    void UpdateAspectTransform();

    // Inspect an HRESULT from a swap-chain or device call. When it is a DXGI
    // device-removed or device-reset code, log the specific
    // GetDeviceRemovedReason and latch m_deviceLost. Any other HRESULT is
    // ignored here and left for the call site to handle. `site` names the
    // originating call for the log line.
    void FlagIfDeviceLost(HRESULT hr, const wchar_t* site);

    // CaptureFormatKind is declared in the public section above so the
    // application's CaptureDevice→renderer subtype routing can use it
    // without including this private section.

    ComPtr<ID3D11Device>           m_device;
    ComPtr<ID3D11DeviceContext>    m_context;
    ComPtr<IDXGISwapChain1>        m_swapChain;

    // Frame-latency waitable object (DXGI 1.3 low-latency technique).
    // Waiting on this handle at the top of every frame allows DXGI to signal
    // exactly when the previous frame has cleared the present queue, so the
    // renderer never queues more than one frame ahead of the display. Default
    // DXGI behavior queues up to 3 frames, costing ~33-50ms of unnecessary
    // latency in this capture-viewer pipeline. With the waitable object plus
    // max latency 1, queue depth caps at 1 frame, saving 1-2 frames of latency.
    //
    // The handle is owned here: closed in the destructor. It's signaled by
    // DXGI when the swap chain is ready for the next Present. The wait
    // happens at the START of BeginFrame.
    HANDLE                         m_frameLatencyWaitable = nullptr;
    // VRR present-rate cap: when > 0 (set from VRR_CAP.txt at init), WaitForFrameReady
    // paces the loop to this Hz instead of the swap-chain waitable, keeping the
    // ALLOW_TEARING present just under the display's VRR max so VRR engages.
    double                         m_vrrCapHz = 0.0;
    bool                           m_presentCapFromMarker = false;
    float                          m_aspectOverride = 0.0f;
    std::chrono::steady_clock::time_point m_lastPresentTime{};

    // Per-phase wall time accumulated between ConsumePhaseTimes calls, for
    // the run loop's pacing diagnostic: the frame-ready wait, the capture
    // upload, and Present. A loop that runs slower than the source shows up
    // here as one phase growing, which separates a blocking Present (display
    // or compositor side) from an upload or wait problem.
    double   m_phaseWaitMs    = 0.0;
    double   m_phaseUploadMs  = 0.0;
    double   m_phasePresentMs = 0.0;
    uint32_t m_phaseWaits     = 0;
    uint32_t m_phasePresents  = 0;

    // Frame-latency telemetry. Averaged window of recent end-to-end render
    // times (BeginFrame to Present), reported into PushSettingsState so the
    // UI can show a live "render ms" number alongside capture latency.
    LARGE_INTEGER                  m_frameStartQpc{};
    double                         m_renderMsSum = 0.0;
    uint32_t                       m_renderMsCount = 0;
    double                         m_lastReportedRenderMs = 0.0;

    // Real GPU per-frame timing via D3D11_QUERY_TIMESTAMP queries.
    // The ring exists so frame N's readback pulls frame N-3's data, which
    // is guaranteed to be ready without forcing a stall. Three slots is the
    // sweet spot for SetMaximumFrameLatency(1) plus the immediate-mode
    // context's natural one-frame deferral.
    static constexpr int           kGpuQueryRingSize = 3;
    ComPtr<ID3D11Query>            m_gpuQueryDisjoint[kGpuQueryRingSize];
    ComPtr<ID3D11Query>            m_gpuQueryStart   [kGpuQueryRingSize];
    ComPtr<ID3D11Query>            m_gpuQueryEnd     [kGpuQueryRingSize];
    bool                           m_gpuQueryIssued  [kGpuQueryRingSize] = { false, false, false };
    int                            m_gpuQueryWriteSlot     = 0;
    int                            m_gpuQueryFrameCount    = 0;
    bool                           m_gpuQueryActiveThisFrame = false;
    bool                           m_gpuQueryEnabled       = false; // false when CreateGpuTimingQueries failed
    double                         m_lastGpuMs = 0.0;

    ComPtr<ID3D11RenderTargetView> m_rtv;

    // Blend states are owned per renderer so they are released with its
    // device. Any device object that outlives a device-loss rebuild keeps the
    // lost device alive, together with the backbuffer view still bound on its
    // context and therefore the old swap chain; a window carries only one
    // swap chain, so the replacement device's CreateSwapChainForHwnd is then
    // refused on every retry.
    ComPtr<ID3D11BlendState>       m_uiBlendState;
    ComPtr<ID3D11BlendState>       m_diagBlendState;

    // Post-process input target: when m_postInputEnabled is true,
    // DrawCaptureFrame writes its Catmull-Rom + range-expanded output here
    // (sized to the source capture, not the window). An upscaler then reads
    // m_postInputSRV and produces a higher-res result, which the renderer
    // composites back onto the backbuffer via CompositeUpscaledTexture.
    ComPtr<ID3D11Texture2D>          m_postInputTex;
    ComPtr<ID3D11RenderTargetView>   m_postInputRTV;
    ComPtr<ID3D11ShaderResourceView> m_postInputSRV;
    uint32_t                         m_postInputW = 0;
    uint32_t                         m_postInputH = 0;
    bool                             m_postInputEnabled = false;

    // A simple passthrough pixel shader used to composite the upscaled
    // texture onto the backbuffer with aspect-correct letterboxing.
    ComPtr<ID3D11PixelShader>        m_compositePS;
    // Pixel shader for compositing the BGRA8 UI texture into an HDR
    // (FP16 scRGB) backbuffer: samples sRGB and outputs scRGB at ~200 nits.
    // See g_pixelShaderUI in the .cpp for the HLSL source.
    ComPtr<ID3D11PixelShader>        m_pixelShaderUI;

    // Capture texture + SRVs.
    //   BGRA: m_captureSRV samples the texture directly as B8G8R8A8_UNORM.
    //   NV12: m_captureSRV samples the Y plane as R8_UNORM; m_captureSRV_UV
    //         samples the interleaved UV plane as R8G8_UNORM.
    //   P010: m_captureSRV samples the Y plane as R16_UNORM (top 10 bits
    //         used); m_captureSRV_UV samples interleaved UV as R16G16_UNORM.
    ComPtr<ID3D11Texture2D>          m_captureTexture;
    ComPtr<ID3D11ShaderResourceView> m_captureSRV;     // Y plane or BGRA full
    ComPtr<ID3D11ShaderResourceView> m_captureSRV_UV;  // UV plane (NV12/P010)
    ComPtr<ID3D11SamplerState>       m_sampler;
    uint32_t m_captureWidth  = 0;
    uint32_t m_captureHeight = 0;
    bool     m_hasFrame      = false;
    // m_captureFormat tracks the format of the currently-ALLOCATED GPU
    // resources. Set by CreateCaptureResources(_P010). Drives the upload
    // branches in UpdateCaptureTexture and the shader-binding switch in
    // DrawCaptureFrame.
    CaptureFormatKind m_captureFormat = CaptureFormatKind::BGRA;
    // m_sourceFormat is what the APPLICATION declared via SetSourceFormat.
    // When it diverges from m_captureFormat, UpdateCaptureTexture recreates
    // resources to match. Initially equal to m_captureFormat's default so
    // first-frame behavior is well-defined even if SetSourceFormat hasn't
    // been called yet.
    CaptureFormatKind m_sourceFormat    = CaptureFormatKind::BGRA;
    bool              m_sourceFormatSet = false; // becomes true on first SetSourceFormat
    bool              m_sourceTopDown = false; // set by SetSourceRowOrder()
    bool              m_p010LimitedChroma = false;
    bool              m_sourceFullRange = false; // set by SetSourceFullRange()
    bool              m_sourceIsHDR10 = false; // set by SetSourceIsHDR10()

    // Fullscreen quad
    ComPtr<ID3D11VertexShader>   m_vertexShader;
    ComPtr<ID3D11PixelShader>    m_pixelShaderBGRA;
    ComPtr<ID3D11PixelShader>    m_pixelShaderNV12;
    ComPtr<ID3D11PixelShader>    m_pixelShaderP010;
    ComPtr<ID3D11Buffer>         m_vertexBuffer;
    ComPtr<ID3D11InputLayout>    m_inputLayout;
    ComPtr<ID3D11Buffer>         m_transformCB; // Aspect ratio transform (VS b0)
    ComPtr<ID3D11Buffer>         m_pixelCB;     // Color pipeline knobs (PS b0)

    // Color expansion: lerped between current and target each frame so the
    // toggle is silky rather than a hard pop.
    float m_colorExpansionCurrent = 1.0f;
    float m_colorExpansionTarget  = 1.0f;
    bool  m_colorExpansionInitialized = false;

    // HDR state. When m_hdrEnabled is true the swap chain is R10G10B10A2 +
    // DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 (HDR10: ST.2084 PQ +
    // BT.2020 primaries). The capture pixel shaders (BGRA, NV12, P010) each
    // have their own HDR output branches that PQ-encode for this swap
    // chain; the UI is composited via CompositeUI() above.
    bool  m_hdrEnabled            = false;
    bool  m_hdrDisplaySupported   = false;
    bool  m_hdrEngagedByOS        = false;
    bool  m_hdrDiagMode           = false;   // Ctrl+F4 calibration overlay
    ComPtr<ID3D11PixelShader> m_hdrDiagPS;
    // True when SetHDREnabled landed on the P709 fallback colorspace (driver
    // doesn't support scRGB-BT.2020 surfaces). In this case the HDR pixel
    // shader must apply BT.2020→BT.709 matrix rotation to avoid wide-gamut
    // hue clipping. On P2020, colors stay in BT.2020 and Windows handles
    // the wide-gamut handoff.
    bool  m_hdrUseBT709Matrix     = false;
    float m_displayMinLum         = 0.0f;
    float m_displayMaxLum         = 80.0f;  // safe SDR fallback
    DXGI_FORMAT m_currentSwapFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

    // HDR test pattern shader. Renders an SDR-vs-HDR comparison grid using
    // scRGB values so HDR engagement can be visually confirmed.
    ComPtr<ID3D11PixelShader>        m_hdrTestPS;

    // Recreates the swap chain backbuffer in a new format, preserving size.
    // Used by SetHDREnabled. Internal: public API goes through that.
    bool RecreateSwapChainBuffer(DXGI_FORMAT newFormat);
    // Queries the connected display's HDR capability and current colorspace,
    // populating m_hdrDisplaySupported / m_hdrEngagedByOS / luminance range.
    void QueryHDRDisplayInfo();
    bool CreateHDRTestShader();

    uint32_t m_windowWidth  = 0;
    uint32_t m_windowHeight = 0;
    HWND     m_hwnd = nullptr;

    // Latched when Present or a capture-texture Map returns a DXGI device-
    // removed or device-reset code. Polled and cleared by ConsumeDeviceLost.
    bool     m_deviceLost = false;
};

} // namespace NitLink
