#pragma once

#include <windows.h>
#include <string>
#include <utility>
#include <cstdint>
#include <functional>
#include "playback_power.h"

namespace NitLink {

class Window {
public:
    struct Desc {
        std::wstring title  = L"NitLink";
        uint32_t     width  = 1920;
        uint32_t     height = 1080;
        bool         darkMode = true;
    };

    // Input event callbacks -- set by the application to route mouse/keyboard
    // events to the SettingsPanel or other UI consumers. All optional; nullptr
    // means "ignore that event type".
    struct InputCallbacks {
        std::function<void(int x, int y)>           onMouseMove;
        std::function<void(int x, int y)>           onMouseDown;
        std::function<void(int x, int y)>           onMouseUp;
        std::function<void(int delta)>              onMouseWheel;
        std::function<void(int vk)>                 onKeyDown;
    };

    explicit Window(PlaybackPowerRequest::SetStateFn setPowerState = ::SetThreadExecutionState);
    ~Window();

    bool Create(HINSTANCE hInstance, const Desc& desc);
    void Show(int nCmdShow);
    void SetTitle(const std::wstring& title);
    void SetIcon(const std::wstring& iconPath);
    void SetFullscreen(bool fullscreen);
    void SetPiP(bool enabled, uint32_t width, uint32_t height, float opacity);
    bool SetPiPOpacity(float opacity);
    void SetPreventSleep(bool enabled);
    void SetVideoAvailable(bool available);
    // Hides the PC pointer over live video in the active window. The pointer
    // stays visible over the settings panel, in Picture-in-Picture and
    // whenever another window is in front.
    void SetHidePcCursorDuringGameplay(bool enabled);
    void SetGameplaySettingsVisible(bool visible);
    bool IsPreventingSleep() const { return m_playbackPower.IsActive(); }

    // Sets where SetPiP will position the PiP window the next time it
    // enters PiP mode. Pass -1 / -1 to fall back to the default
    // (bottom-right of the monitor's work area). Persists in member
    // state; Application loads it from Config at startup and re-applies
    // it on every TogglePiP / NudgePiP.
    void SetPreferredPiPPosition(int32_t x, int32_t y) {
        m_pipPreferredX = x;
        m_pipPreferredY = y;
    }

    // Move the PiP window by (dx, dy) pixels, clamped to the current
    // monitor's work area. No-op when not currently in PiP. Returns true
    // if the window was actually moved.
    bool NudgePiP(int dx, int dy);

    // The applied size stays within config bounds and the current work area.
    // Returns false outside PiP, at a size limit, or if placement fails.
    bool ResizePiP(int dw, int dh, bool keepAspect = false);
    void SetPiPResizeCallback(std::function<void()> cb) { m_onPiPResize = std::move(cb); }

    // Read back the PiP window's current top-left screen position so the
    // caller can persist it. Returns false (and leaves outputs untouched)
    // when not currently in PiP.
    bool GetPiPPosition(int32_t& outX, int32_t& outY) const;

    void SetInputCallbacks(InputCallbacks cb) { m_inputCb = std::move(cb); }
    // Synchronous WM_MOVE notifier. Set this to a function that
    // repositions the WebView2 popup (or anything else that must follow
    // the main window's screen position). Called DIRECTLY from inside
    // WndProc on every WM_MOVE (not deferred through the flag below),
    // because during a title-bar drag Windows enters a modal message
    // loop and the application's PeekMessage pump is blocked. The flag
    // wouldn't get consumed until the drag ended, leaving the popup
    // stranded mid-drag.
    void SetMoveCallback(std::function<void()> cb) { m_onMove = std::move(cb); }
    void SetCloseCallback(std::function<void()> cb) { m_onClose = std::move(cb); }

    HWND GetHWND() const { return m_hwnd; }
    std::pair<uint32_t, uint32_t> GetClientSize() const;
    std::pair<uint32_t, uint32_t> GetWindowedClientSize() const { return m_windowedClientSize; }
    bool WasResized() const { return m_wasResized; }
    void AcknowledgeResize() { m_wasResized = false; }
    // Defer monitor-crossing detection to the run loop. Owner movement still
    // reaches the settings host synchronously through the move callback.
    bool WasMoved() const { return m_wasMoved; }
    void AcknowledgeMove() { m_wasMoved = false; }
    bool WasDisplayChanged() const { return m_displayChanged; }
    void AcknowledgeDisplayChange() { m_displayChanged = false; }

    // Window procedure
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    void UpdatePlaybackPower();
    bool RefreshGameplayCursor(bool reapply = false);
    bool GetPiPWorkArea(RECT& work) const;
    bool ApplyPiPRect(const RECT& rect);
    int HitTestPiPResize(POINT screenPoint) const;
    bool BeginPiPResize(POINT screenPoint);
    void UpdatePiPResize(POINT screenPoint);
    void EndPiPResize();

    HWND       m_hwnd = nullptr;
    PlaybackPowerRequest m_playbackPower;
    bool m_preventSleep = true;
    bool m_videoAvailable = false;
    bool m_hidePcCursorDuringGameplay = false;
    bool m_gameplaySettingsVisible = false;
    bool m_gameplayCursorHidden = false;
    bool m_gameplayCursorActive = false;
    HINSTANCE  m_hInstance = nullptr;
    bool       m_isFullscreen = false;
    bool       m_wasResized = false;
    bool       m_wasMoved = false;
    bool       m_displayChanged = false;
    RECT       m_windowedRect{}; // Stored position for fullscreen restore
    DWORD      m_windowedStyle = 0;
    std::pair<uint32_t, uint32_t> m_windowedClientSize{};

    // PiP state
    bool     m_isPiP = false;
    uint32_t m_pipWidth = 480;
    uint32_t m_pipHeight = 270;
    double   m_pipScaleAspect = 0.0;
    int      m_pipResizeEdges = 0;
    RECT     m_pipResizeStart{};
    RECT     m_pipResizeWork{};
    POINT    m_pipResizeMouse{};
    // Preferred top-left for the next SetPiP(enabled=true) call. -1 / -1
    // = use the monitor's bottom-right default. Updated by NudgePiP and
    // SetPreferredPiPPosition.
    int32_t  m_pipPreferredX = -1;
    int32_t  m_pipPreferredY = -1;

    InputCallbacks m_inputCb;
    std::function<void()> m_onClose;
    std::function<void()> m_onMove;
    std::function<void()> m_onPiPResize;
};

} // namespace NitLink
