#include "app/WebViewSettings.h"
#include "app/settings_message.h"
#include "app/webview_lifecycle.h"
#include "app/webview_policy.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::Callback;

struct WebViewSettingsTestAccess {
    static void QueueFinalSliders(WebViewSettings& host) {
        const auto now = GetTickCount64();
        while (host.m_messageBudget.Accept(now)) {}
        host.m_pendingSliders.Store(0, LR"({"action":"setVolume","value":0.234})");
        host.m_pendingSliders.Store(1, LR"({"action":"setPiPOpacity","value":0.567})");
    }
    static bool DeadlineUnarmed(const WebViewSettings& host) { return host.m_startTime == 0; }
    static void ExpireReadyDeadline(WebViewSettings& host) {
        host.m_startTime = GetTickCount64() - NitLink::kSettingsReadyTimeoutMs;
    }
    static unsigned Restarts(const WebViewSettings& host) { return host.m_restartAttempts; }
    static ICoreWebView2* View(WebViewSettings& host) { return host.m_webview.get(); }
    static ICoreWebView2Controller* Controller(WebViewSettings& host) { return host.m_controller.get(); }
};

namespace {
void Check(bool result, const char* label) {
    if (!result) throw std::runtime_error(label);
    std::cout << "PASS " << label << '\n';
}
void Hr(HRESULT hr) {
    if (FAILED(hr)) throw std::runtime_error("WebView API failed: " + std::to_string(hr));
}
bool PumpUntil(const std::function<bool()>& predicate, int milliseconds = 10000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    do {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (predicate()) return true;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    } while (std::chrono::steady_clock::now() < end);
    return false;
}
std::wstring Script(ICoreWebView2* view, const wchar_t* script) {
    struct Result { bool done = false; HRESULT hr = E_PENDING; std::wstring value; };
    const auto result = std::make_shared<Result>();
    Hr(view->ExecuteScript(script, Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
        [result](HRESULT hr, LPCWSTR json) -> HRESULT {
            result->hr = hr;
            if (json) result->value = json;
            result->done = true;
            return S_OK;
        }).Get()));
    if (!PumpUntil([&] { return result->done; })) throw std::runtime_error("script timed out");
    Hr(result->hr);
    return result->value;
}
}

int main(int argc, char** argv) {
    const bool expectMissingAssets = argc == 2 && std::string(argv[1]) == "--expect-missing-assets";
    const bool expectReadyTimeout = argc == 2 && std::string(argv[1]) == "--expect-ready-timeout";
    // This standalone host has no capture or renderer code. A disposable profile
    // and software-only browser flags keep the viewer's profile and GPU untouched.
    const auto profile = std::filesystem::temp_directory_path() /
        (L"NitLink-WebView-test-" + std::to_wstring(GetCurrentProcessId()));
    SetEnvironmentVariableW(L"WEBVIEW2_USER_DATA_FOLDER", profile.c_str());
    SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS",
        L"--disable-gpu --disable-gpu-compositing --disable-background-networking");
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    HWND window = CreateWindowExW(0, L"STATIC", L"NitLink settings test", WS_OVERLAPPEDWINDOW,
        0, 0, 800, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    int result = 0;
    try {
        Check(window != nullptr, "hidden test window");
        if (expectMissingAssets || expectReadyTimeout) {
            WebViewSettings incomplete;
            std::wstring failure;
            unsigned failures = 0;
            incomplete.SetFailureHandler([&](const std::wstring& reason) { failure = reason; ++failures; });
            Check(incomplete.Initialize(window, 800, 600), "incomplete-package initialization dispatched");
            incomplete.NavigateToMenu();
            incomplete.Show(true);
            Check(!incomplete.IsVisible(), "starting page remains hidden until trusted ready");
            Check(WebViewSettingsTestAccess::DeadlineUnarmed(incomplete),
                  "initialization does not start a timer before pumping");
            Check(PumpUntil([&] {
                incomplete.CheckHealth();
                auto* current = WebViewSettingsTestAccess::View(incomplete);
                if (expectReadyTimeout && current) {
                    wil::unique_cotaskmem_string source;
                    if (SUCCEEDED(current->get_Source(&source)) && source &&
                        NitLink::WebViewPolicy::IsTrustedDocument(source.get()) &&
                        Script(current, L"document.readyState") == L"\"complete\"") {
                        WebViewSettingsTestAccess::ExpireReadyDeadline(incomplete);
                        incomplete.CheckHealth();
                    }
                }
                return !failure.empty() && !incomplete.IsStarting();
            }, 20000), "failed startup stops after bounded automatic retries");
            Check(failures == 3 && WebViewSettingsTestAccess::Restarts(incomplete) == 2,
                  "initial attempt plus exactly two restarts");
            Check(failure == (expectReadyTimeout ? L"toast.settingsTimeout" : L"toast.settingsFiles") &&
                  !incomplete.IsReady() && !incomplete.IsStarting(), "failure identifies localized repair reason");
            incomplete.Show(true);
            Check(!incomplete.IsVisible(), "failed package never opens an empty panel");
            incomplete.Shutdown();
        } else {
        std::vector<std::wstring> messages;
        WebViewSettings host;
        Check(host.Initialize(window, 800, 600), "host initialization dispatched");
        Check(WebViewSettingsTestAccess::DeadlineUnarmed(host), "healthy startup timer waits for message pumping");
        host.SetMessageHandler([&](const std::wstring& json) { messages.push_back(json); });
        host.NavigateToMenu();
        Check(PumpUntil([&] { return !messages.empty(); }, 15000), "packaged menu ready via real bridge");
        Check(NitLink::ParseSettingsMessage(messages.front()).has_value(), "ready payload schema");
        auto* view = WebViewSettingsTestAccess::View(host);
        Check(view != nullptr, "secured WebView available");
        auto view2 = wil::com_ptr<ICoreWebView2>(view).try_query<ICoreWebView2_2>();
        if (view2) {
            wil::com_ptr<ICoreWebView2Environment> env;
            Hr(view2->get_Environment(&env));
            wil::unique_cotaskmem_string version;
            Hr(env->get_BrowserVersionString(&version));
            std::wcout << L"Runtime " << version.get() << L'\n';
        }
        wil::com_ptr<ICoreWebView2Settings> settings;
        Hr(view->get_Settings(&settings));
        BOOL enabled = TRUE;
        Hr(settings->get_AreHostObjectsAllowed(&enabled)); Check(!enabled, "host objects disabled");
        Hr(settings->get_AreDevToolsEnabled(&enabled)); Check(!enabled, "developer tools disabled");
        Hr(settings->get_AreDefaultScriptDialogsEnabled(&enabled)); Check(!enabled, "dialogs disabled");
        auto controller4 = wil::com_ptr<ICoreWebView2Controller>(WebViewSettingsTestAccess::Controller(host))
            .try_query<ICoreWebView2Controller4>();
        Hr(controller4->get_AllowExternalDrop(&enabled)); Check(!enabled, "external drop disabled");
        Check(Script(view, LR"JS((() => {
            window.__violations = [];
            document.addEventListener('securitypolicyviolation', e => __violations.push(e.violatedDirective));
            window.__injected = false;
            const s = document.createElement('script'); s.textContent = 'window.__injected=true'; document.body.append(s);
            const f = document.createElement('iframe'); f.srcdoc = '<script>parent.__injected=true<\/script>'; document.body.append(f);
            const remoteFrame = document.createElement('iframe'); remoteFrame.src = 'https://blocked.invalid/frame'; document.body.append(remoteFrame);
            fetch('https://blocked.invalid/').catch(() => {});
            try { new Worker('assets/menu/menu.js'); } catch (_) {}
            return document.querySelector('h1').textContent.length > 0;
        })())JS") == L"true", "localized UI loaded");
        PumpUntil([] { return false; }, 300);
        std::wcout << L"CSP observations " << Script(view, L"JSON.stringify({injected:window.__injected,violations:window.__violations})") << L'\n';
        Check(Script(view, L"!window.__injected && __violations.some(x=>x.startsWith('script-src')) && __violations.includes('frame-src') && __violations.includes('connect-src') && __violations.includes('worker-src')") == L"true",
              "CSP blocks inline injection, frames, connections and workers");
        host.PostMessage(LR"({"state":{"locale":"zh-TW","languagePreference":"zh-TW","audioVolume":0.75}})");
        Check(PumpUntil([&] { return Script(view, L"document.documentElement.lang") == L"\"zh-TW\""; }),
              "native state reaches trusted page");
        for (const auto* locale : {L"en-US", L"zh-TW"}) {
            const std::wstring prefix = std::wstring(L"{\"state\":{\"locale\":\"") + locale +
                L"\",\"negotiatedWidth\":1920,\"negotiatedHeight\":1080,";
            host.PostMessage((prefix + L"\"negotiatedFps\":120,\"negotiatedFpsNumerator\":120,"
                              L"\"negotiatedFpsDenominator\":1}}").c_str());
            Check(PumpUntil([&] { return Script(view,
                L"document.getElementById('meta-resolution').textContent === '1920x1080 / 120 FPS'") == L"true"; }),
                "negotiated integer rate displays in both languages");
            host.PostMessage((prefix + L"\"negotiatedFps\":119,\"negotiatedFpsNumerator\":120000,"
                              L"\"negotiatedFpsDenominator\":1001}}").c_str());
            Check(PumpUntil([&] { return Script(view,
                L"document.getElementById('meta-resolution').textContent === '1920x1080 / 119.88 FPS'") == L"true"; }),
                "negotiated fractional rate displays without integer truncation");
            host.PostMessage((prefix + L"\"negotiatedFps\":0,\"negotiatedFpsNumerator\":0,"
                              L"\"negotiatedFpsDenominator\":1}}").c_str());
            Check(PumpUntil([&] { return Script(view,
                L"document.getElementById('meta-resolution').textContent === '1920x1080 / -- FPS'") == L"true"; }),
                "missing negotiated rate remains unknown while resolution stays visible");
        }
        host.PostMessage(LR"({"state":{"noSignalMode":"image","noSignalImage":"C:\\photo.png","noSignalImageAvailable":true}})");
        Check(PumpUntil([&] { return Script(view, L"document.getElementById('no-signal-mode-val').textContent === window.NitLinkLocales['zh-TW']['value.customImage']") == L"true"; }),
              "custom image label available");
        Script(view, L"window.__neutralImageLabel = document.getElementById('no-signal-mode-val').textContent");
        host.PostMessage(LR"({"state":{"noSignalMode":"image","noSignalImage":"","noSignalImageAvailable":false,"configWarning":"Settings will not be saved."}})");
        Check(PumpUntil([&] {
            return Script(view, L"document.getElementById('no-signal-mode-val').textContent === window.__neutralImageLabel") == L"true";
        }), "unchosen custom image keeps neutral localized label");
        Check(Script(view, L"document.getElementById('config-warning').textContent === 'Settings will not be saved.' && document.getElementById('config-warning').getBoundingClientRect().height > 0") == L"true",
              "config warning remains visibly in settings");
        Check(Script(view, L"document.getElementById('no-signal-mode-val').textContent === window.NitLinkLocales['zh-TW']['value.customImage']") == L"true",
              "applied empty-image state has no unavailable label");
        host.PostMessage(LR"({"state":{"configWarning":""}})");
        Check(PumpUntil([&] { return Script(view, L"document.getElementById('config-warning').hidden") == L"true"; }),
              "resolved config warning clears");
        const size_t count = messages.size();
        Script(view, L"document.querySelector('[data-action=toggleHDR]').click()");
        Check(PumpUntil([&] { return messages.size() > count; }), "menu control posts through native bridge");
        Check(NitLink::ParseSettingsMessage(messages.back()).has_value(), "menu control payload schema");

        const size_t beforeAuto = messages.size();
        Script(view, L"chrome.webview.postMessage({action:'setCaptureFormatOverride',value:{width:0,height:0,fps:0,fpsNumerator:0,fpsDenominator:1,format:''}})");
        Check(PumpUntil([&] { return messages.size() > beforeAuto; }), "real WebView serializer delivers Auto payload");
        const auto automatic = NitLink::ParseSettingsMessage(messages.back());
        Check(automatic && automatic->action == L"setCaptureFormatOverride" &&
              automatic->format.isFullAuto() && automatic->format.fpsDenominator == 1,
              "real Auto payload preserves zero numerator and unit denominator");

        const size_t beforeUntrusted = messages.size();
        Script(view, LR"JS(
            window.__nativeProbe = false;
            chrome.webview.addEventListener('message', e => { if (e.data.probe) __nativeProbe = true; });
            history.replaceState(null, '', '#untrusted');
        )JS");
        // History notifications reach the host asynchronously; this remains the
        // same loaded document until the host observes the changed source.
        Check(PumpUntil([&] {
            wil::unique_cotaskmem_string source;
            return SUCCEEDED(view->get_Source(&source)) && source &&
                std::wstring_view(source.get()).ends_with(L"#untrusted");
        }), "host observes changed source");
        Script(view, L"chrome.webview.postMessage({action:'toggleHDR'})");
        host.PostMessage(LR"({"probe":true})");
        PumpUntil([] { return false; }, 100);
        Check(messages.size() == beforeUntrusted, "bridge rejects changed document source");
        Check(Script(view, L"window.__nativeProbe") == L"false", "state withheld from changed document");
        Script(view, L"history.replaceState(null, '', '/nitlink-menu.html')");
        Check(PumpUntil([&] {
            wil::unique_cotaskmem_string source;
            return SUCCEEDED(view->get_Source(&source)) && source &&
                NitLink::WebViewPolicy::IsTrustedDocument(source.get());
        }), "host observes restored source");
        host.PostMessage(LR"({"probe":true})");
        Check(PumpUntil([&] { return Script(view, L"window.__nativeProbe") == L"true"; }), "state resumes only at exact document");

        auto deniedResource = std::make_shared<bool>(false);
        EventRegistrationToken resourceToken{};
        Hr(view2->add_WebResourceResponseReceived(Callback<ICoreWebView2WebResourceResponseReceivedEventHandler>(
            [deniedResource](ICoreWebView2*, ICoreWebView2WebResourceResponseReceivedEventArgs* args) -> HRESULT {
                wil::com_ptr<ICoreWebView2WebResourceRequest> request;
                wil::unique_cotaskmem_string uri;
                wil::com_ptr<ICoreWebView2WebResourceResponseView> response;
                int status = 0;
                if (SUCCEEDED(args->get_Request(&request)) && SUCCEEDED(request->get_Uri(&uri)) && uri &&
                    std::wstring_view(uri.get()) == L"https://nitlink.invalid/nitlink.json" &&
                    SUCCEEDED(args->get_Response(&response)) && SUCCEEDED(response->get_StatusCode(&status)))
                    *deniedResource = status == 403;
                return S_OK;
            }).Get(), &resourceToken));
        Script(view, L"const probe = new Image(); probe.src='https://nitlink.invalid/nitlink.json'; document.body.append(probe)");
        Check(PumpUntil([&] { return *deniedResource; }), "unlisted local resource receives native 403");
        Hr(view2->remove_WebResourceResponseReceived(resourceToken));

        auto navigationCancelled = std::make_shared<bool>(false);
        EventRegistrationToken navToken{};
        Hr(view->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
            [navigationCancelled](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                BOOL cancel = FALSE;
                const HRESULT hr = args->get_Cancel(&cancel);
                *navigationCancelled = SUCCEEDED(hr) && cancel;
                return hr;
            }).Get(), &navToken));
        for (auto uri : {L"file:///C:/NitLink-security-fixture/blocked.html", L"https://blocked.invalid/",
             L"https://nitlink.invalid/locales/en-US.js", L"https://nitlink.invalid/nitlink-menu.html?x",
             L"data:text/html,blocked", L"about:blank"}) {
            *navigationCancelled = false;
            Hr(view->Navigate(uri));
            Check(PumpUntil([&] { return *navigationCancelled; }), "host cancels untrusted navigation");
        }
        Hr(view->remove_NavigationStarting(navToken));
        auto popupHandled = std::make_shared<bool>(false);
        EventRegistrationToken popupToken{};
        Hr(view->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [popupHandled](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                BOOL handled = FALSE;
                const HRESULT hr = args->get_Handled(&handled);
                *popupHandled = SUCCEEDED(hr) && handled;
                return hr;
            }).Get(), &popupToken));
        Script(view, L"window.open('https://blocked.invalid/')");
        Check(PumpUntil([&] { return *popupHandled; }), "unlisted popup handled without opening a browser");
        Hr(view->remove_NewWindowRequested(popupToken));
        auto permissionDenied = std::make_shared<bool>(false);
        EventRegistrationToken permissionToken{};
        Hr(view->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>(
            [permissionDenied](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                COREWEBVIEW2_PERMISSION_STATE state = COREWEBVIEW2_PERMISSION_STATE_DEFAULT;
                const HRESULT hr = args->get_State(&state);
                *permissionDenied = SUCCEEDED(hr) && state == COREWEBVIEW2_PERMISSION_STATE_DENY;
                return hr;
            }).Get(), &permissionToken));
        Script(view, L"window.__permission='pending'; Notification.requestPermission().then(result=>window.__permission=result)");
        Check(PumpUntil([&] { return Script(view, L"window.__permission") == L"\"denied\""; }), "permission denied");
        Check(*permissionDenied, "native permission handler denies the request");
        Hr(view->remove_PermissionRequested(permissionToken));
        const size_t beforeBurst = messages.size();
        Script(view, L"for(let i=0;i<2000;i++) chrome.webview.postMessage({action:'ready'})");
        PumpUntil([] { return false; }, 300);
        Check(messages.size() > beforeBurst && messages.size() - beforeBurst < 2000,
              "native bridge limits a message burst");
        Script(view, L"for(let i=0;i<2000;i++) { chrome.webview.postMessage({action:'setVolume',value:i/2000}); chrome.webview.postMessage({action:'setPiPOpacity',value:0.1+0.9*i/2000}); } chrome.webview.postMessage({action:'setVolume',value:0.321}); chrome.webview.postMessage({action:'setPiPOpacity',value:0.654})");
        Check(PumpUntil([&] {
            host.DispatchPendingMessages();
            double volume = -1, opacity = -1;
            for (const auto& json : messages) {
                const auto message = NitLink::ParseSettingsMessage(json);
                if (message && message->action == L"setVolume") volume = message->number;
                if (message && message->action == L"setPiPOpacity") opacity = message->number;
            }
            return volume == 0.321 && opacity == 0.654;
        }), "both final slider values survive an exhausted message budget");
        WebViewSettingsTestAccess::QueueFinalSliders(host);
        const auto beforeClose = messages.size();
        host.DispatchPendingMessages(/*closing=*/true);
        double closingVolume = -1, closingOpacity = -1;
        for (size_t i = beforeClose; i < messages.size(); ++i) {
            const auto parsed = NitLink::ParseSettingsMessage(messages[i]);
            if (parsed && parsed->action == L"setVolume") closingVolume = parsed->number;
            if (parsed && parsed->action == L"setPiPOpacity") closingOpacity = parsed->number;
        }
        Check(messages.size() == beforeClose + 2 && closingVolume == 0.234 && closingOpacity == 0.567,
              "native close commits only the two pending final slider values");
        std::wstring failure;
        host.SetFailureHandler([&](const std::wstring& reason) { failure = reason; });
        UINT32 browserPid = 0;
        Hr(view->get_BrowserProcessId(&browserPid));
        Check(browserPid != 0 && browserPid != GetCurrentProcessId(), "isolated test browser PID");
        HANDLE browser = OpenProcess(PROCESS_TERMINATE, FALSE, browserPid);
        Check(browser != nullptr, "test browser process handle");
        const bool terminated = TerminateProcess(browser, 1) != FALSE;
        CloseHandle(browser);
        Check(terminated && PumpUntil([&] { return !failure.empty(); }), "real browser process failure reaches host");
        Check(!host.IsReady() && !host.IsVisible() && failure == L"toast.settingsProcessFailed" &&
              failure == host.InitializationError(), "failed host reports localized reason and closes panel");
        host.Show(true);
        Check(!host.IsVisible(), "restarting host stays hidden");
        Check(PumpUntil([&] { host.CheckHealth(); return host.IsReady(); }),
              "browser crash automatically restarts the secured host");
        auto* recovered = WebViewSettingsTestAccess::View(host);
        Check(recovered && host.InitializationError().empty() &&
              Script(recovered, L"document.documentElement.lang") == L"\"en-US\"",
              "restarted host reloads the packaged menu");
        wil::com_ptr<ICoreWebView2Settings> recoveredSettings;
        Hr(recovered->get_Settings(&recoveredSettings));
        Hr(recoveredSettings->get_AreHostObjectsAllowed(&enabled));
        Check(!enabled, "restart reapplies host-object security restriction");
        host.Shutdown();

        WebViewSettings navigationHost;
        std::wstring navigationFailure;
        navigationHost.SetFailureHandler([&](const std::wstring& reason) { navigationFailure = reason; });
        Check(navigationHost.Initialize(window, 800, 600), "navigation failure fixture starts");
        navigationHost.NavigateToMenu();
        Check(PumpUntil([&] { navigationHost.CheckHealth(); return navigationHost.IsReady(); }),
              "navigation fixture reaches trusted ready");
        auto* navigationView = WebViewSettingsTestAccess::View(navigationHost);
        EventRegistrationToken cancelToken{};
        Hr(navigationView->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                return args->put_Cancel(TRUE);
            }).Get(), &cancelToken));
        navigationHost.NavigateToMenu();
        Check(PumpUntil([&] { return !navigationFailure.empty(); }) &&
              navigationFailure == L"toast.settingsNavigation" && !navigationHost.IsReady(),
              "failed trusted navigation closes panel and reports reason");
        Check(PumpUntil([&] { navigationHost.CheckHealth(); return navigationHost.IsReady(); }),
              "failed trusted navigation automatically recovers");
        navigationHost.Shutdown();
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        result = 1;
    }
    if (window) DestroyWindow(window);
    CoUninitialize();
    std::wcout << L"Disposable profile: " << profile.c_str() << L'\n';
    return result;
}
