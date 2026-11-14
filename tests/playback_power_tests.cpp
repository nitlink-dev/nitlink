#include "app/config.h"
#include "renderer/window.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using NitLink::PlaybackPowerRequest;
using NitLink::Window;
constexpr EXECUTION_STATE kPlaying = ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED;
EXECUTION_STATE currentState = ES_CONTINUOUS;
std::vector<EXECUTION_STATE> requests;
bool failNextRequest = false;

EXECUTION_STATE WINAPI SetFakeState(EXECUTION_STATE state)
{
    requests.push_back(state);
    if (failNextRequest) {
        failNextRequest = false;
        return 0;
    }
    const auto previous = currentState;
    currentState = state;
    return previous;
}

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
    std::cout << "PASS " << message << '\n';
}

void CheckRequestLifecycle()
{
    requests.clear();
    {
        PlaybackPowerRequest request(SetFakeState);
        Require(request.SetActive(false) && requests.empty(), "idle startup has no power request");
        Require(request.SetActive(true) && currentState == kPlaying,
                "playback requests display and system idle protection");
        const auto count = requests.size();
        for (int i = 0; i < 1000; ++i) request.SetActive(true);
        Require(requests.size() == count, "steady playback does not repeat the Windows call");
        Require(request.SuppressesScreenSaver(SC_SCREENSAVE | 2),
                "ordinary screen saver suppressed with command flags masked");
        Require(!request.SuppressesScreenSaver(SC_SCREENSAVE | SCF_ISSECURE),
                "secure screen saver is not intercepted");
        Require(!request.SuppressesScreenSaver(SC_MONITORPOWER),
                "explicit monitor power commands are not intercepted");
        Require(!request.SuppressesScreenSaver(SC_CLOSE), "close command remains available");
        Require(request.SetActive(false) && currentState == ES_CONTINUOUS,
                "stopping playback clears both requirements");
        Require(!request.SuppressesScreenSaver(SC_SCREENSAVE),
                "idle screen saver is allowed");
        failNextRequest = true;
        Require(!request.SetActive(true) && !request.IsActive(),
                "failed activation is not reported as active");
        Require(request.SetActive(true), "activation retries after failure");
        failNextRequest = true;
        Require(!request.SetActive(false) && request.IsActive(),
                "failed release retains ownership for retry");
        Require(request.SetActive(false), "release retries after failure");
        Require(request.SetActive(true), "playback can restart");
    }
    Require(currentState == ES_CONTINUOUS, "destruction releases an active request");
}

void CheckWindowLifecycle()
{
    const auto instance = GetModuleHandleW(nullptr);
    {
        Window window(SetFakeState);
        Window::Desc desc;
        desc.width = 640;
        desc.height = 360;
        Require(window.Create(instance, desc), "test window creates");
        window.SetVideoAvailable(true);
        Require(!window.IsPreventingSleep(), "hidden window does not prevent sleep");
        window.Show(SW_SHOWNOACTIVATE);
        Require(window.IsPreventingSleep(), "visible video prevents sleep");
        Require(SendMessageW(window.GetHWND(), WM_SYSCOMMAND, SC_SCREENSAVE, 0) == 0,
                "window consumes an ordinary screen-saver request during playback");

        window.SetPreventSleep(false);
        Require(!window.IsPreventingSleep(), "user can disable protection while playing");
        window.SetPreventSleep(true);
        Require(window.IsPreventingSleep(), "user can enable protection while playing");
        window.SetVideoAvailable(false);
        Require(!window.IsPreventingSleep(), "no signal releases the request");
        window.SetVideoAvailable(true);
        Require(window.IsPreventingSleep(), "signal return reacquires the request");

        ShowWindow(window.GetHWND(), SW_MINIMIZE);
        Require(!window.IsPreventingSleep(), "minimize releases before the next video update");
        window.SetVideoAvailable(true);
        Require(!window.IsPreventingSleep(), "capture while minimized does not reacquire");
        window.Show(SW_RESTORE);
        Require(window.IsPreventingSleep(), "restoring visible video reacquires");
        ShowWindow(window.GetHWND(), SW_HIDE);
        Require(!window.IsPreventingSleep(), "hide releases before the next video update");
        window.SetVideoAvailable(true);
        Require(!window.IsPreventingSleep(), "hidden capture does not reacquire");
        window.Show(SW_SHOWNOACTIVATE);
        Require(window.IsPreventingSleep(), "showing video again reacquires");

        window.SetFullscreen(true);
        Require(window.IsPreventingSleep(), "fullscreen video remains protected");
        window.SetFullscreen(false);
        window.SetPiP(true, 480, 270, 1.0f);
        Require(window.IsPreventingSleep(), "picture-in-picture remains protected");
        window.SetPiP(false, 0, 0, 1.0f);
        const auto count = requests.size();
        for (int i = 0; i < 1000; ++i) window.SetVideoAvailable(true);
        Require(requests.size() == count, "paused video keeps protection without repeated requests");

        SendMessageW(window.GetHWND(), WM_CLOSE, 0, 0);
        Require(!window.IsPreventingSleep() && currentState == ES_CONTINUOUS,
                "closing the native window releases immediately");
    }
    Require(UnregisterClassW(L"NitLinkWindowClass", instance) != FALSE,
            "closed test window class releases");
    {
        Window window(SetFakeState);
        Window::Desc desc;
        Require(window.Create(instance, desc), "second test window creates");
        window.Show(SW_SHOWNOACTIVATE);
        Require(!window.IsPreventingSleep(), "visible startup without video remains idle");
        window.SetVideoAvailable(true);
        Require(window.IsPreventingSleep(), "second window acquires during playback");
    }
    Require(currentState == ES_CONTINUOUS, "window destruction releases without a close message");
    Require(UnregisterClassW(L"NitLinkWindowClass", instance) != FALSE,
            "second test window class releases");
    MSG message{};
    while (PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {}
}

void CheckConfig(const std::filesystem::path& path)
{
    NitLink::Config config;
    Require(config.preventSleep, "new configurations default to preventing sleep");
    Require(!config.hidePcCursorDuringGameplay, "new configurations keep the PC cursor visible");
    {
        std::ofstream legacy(path);
        legacy << "window_width = 1280\n";
    }
    Require(config.Load(path.string()) && config.preventSleep,
            "existing configurations without the key keep the default");
    Require(!config.hidePcCursorDuringGameplay,
            "existing configurations without the cursor key keep the cursor visible");
    for (const bool enabled : {false, true}) {
        config.preventSleep = enabled;
        Require(config.Save(path.string()), "sleep preference saves");
        NitLink::Config reloaded;
        reloaded.preventSleep = !enabled;
        Require(reloaded.Load(path.string()) && reloaded.preventSleep == enabled,
                "sleep preference survives restart");
    }
    for (const bool enabled : {true, false}) {
        config.hidePcCursorDuringGameplay = enabled;
        Require(config.Save(path.string()), "cursor preference saves");
        NitLink::Config reloaded;
        reloaded.hidePcCursorDuringGameplay = !enabled;
        Require(reloaded.Load(path.string()) && reloaded.hidePcCursorDuringGameplay == enabled,
                "cursor preference survives restart");
    }
}

} // namespace

int main()
{
    // An inactive desktop keeps window transitions off the user's screen.
    // Power calls are injected so these checks never alter host idle behavior.
    const std::wstring name = L"NitLinkPlaybackPowerTests-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop || !SetThreadDesktop(desktop)) {
        std::cerr << "FAIL Could not create isolated test desktop\n";
        return 1;
    }
    const auto configPath = std::filesystem::temp_directory_path() / (name + L".ini");
    int result = 0;
    try {
        CheckRequestLifecycle();
        CheckWindowLifecycle();
        CheckConfig(configPath);
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        result = 1;
    }
    std::error_code ignored;
    std::filesystem::remove(configPath, ignored);
    return result;
}
