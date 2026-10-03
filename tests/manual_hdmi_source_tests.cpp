#include "app/config.h"
#include "app/hdmi_source_presence.h"
#include "app/settings_message.h"
#include "app/capture_output_policy.h"

#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

using namespace NitLink;

namespace {
size_t checks = 0;
void Check(bool condition, const char* label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
}
std::string Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
std::string WithoutIdentity(const std::string& contents) {
    std::istringstream input(contents);
    std::string line, result;
    while (std::getline(input, line))
        if (!line.starts_with("manual_hdmi_source")) result += line + '\n';
    return result;
}
struct TemporaryConfig {
    std::filesystem::path directory = std::filesystem::temp_directory_path() /
        ("NitLink-manual-source-" + std::to_string(GetCurrentProcessId()) +
         "-" + std::to_string(GetTickCount64()));
    std::filesystem::path path = directory / "nitlink.json";
    TemporaryConfig() { std::filesystem::create_directory(directory); }
    ~TemporaryConfig() {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
};

void TestLabels() {
    for (const auto& [value, label] : kManualHdmiSources) {
        Check(IsValidManualHdmiSource(value), "canonical value accepted");
        const std::wstring wide(value.begin(), value.end());
        Check(IsValidManualHdmiSource(std::wstring_view(wide)), "bridge canonical value accepted");
        if (value != "auto")
            Check(GetEffectiveHdmiSourceLabel(value, L"detected", L"SPD") == label,
                  "manual label overrides both automatic detectors");
    }
    Check(GetEffectiveHdmiSourceLabel("ps5") == L"PS5", "PS5");
    Check(GetEffectiveHdmiSourceLabel("ps4") == L"PS4", "PS4");
    Check(GetEffectiveHdmiSourceLabel("switch2") == L"Switch 2", "Switch 2");
    Check(GetEffectiveHdmiSourceLabel("switch") == L"Switch", "Switch");
    Check(GetEffectiveHdmiSourceLabel("xbox_series") == L"Xbox Series X|S", "Xbox Series X|S");
    Check(GetEffectiveHdmiSourceLabel("xbox_one") == L"Xbox One", "Xbox One");
    Check(GetEffectiveHdmiSourceLabel("pc") == L"PC", "PC");
    Check(GetEffectiveHdmiSourceLabel("other", {}, {}, L" Steam Deck ") == L"Steam Deck",
          "Other custom name trimmed");
    Check(GetEffectiveHdmiSourceLabel("other", {}, {}, L"   ") == L"Other",
          "empty Other name has English fallback");
    Check(GetEffectiveHdmiSourceLabel("auto", L"4K S detected", L"4K X SPD") == L"4K S detected",
          "automatic detector priority preserved");
    Check(GetEffectiveHdmiSourceLabel("auto", {}, L"4K X SPD") == L"4K X SPD",
          "automatic 4K X SPD preserved");
    Check(GetEffectiveHdmiSourceLabel("auto").empty(), "unknown identity stays empty");
    Check(GetEffectiveHdmiSourceLabel("invalid", L"detected") == L"detected",
          "invalid identity safely uses automatic detector");
    Check(GetEffectiveHdmiSourceLabel("auto", L"PS5", {}, L"Steam Deck") == L"PS5",
          "custom ignored in Auto");
    Check(GetEffectiveHdmiSourceLabel("ps4", {}, {}, L"Steam Deck") == L"PS4",
          "custom ignored for fixed sources");
}

void TestCustomValidation() {
    Check(NormalizeManualHdmiSourceCustom(" Steam Deck ") == std::optional<std::string>("Steam Deck"),
          "trim custom");
    Check(NormalizeManualHdmiSourceCustom("   ") == std::optional<std::string>(""),
          "whitespace custom is legal");
    Check(NormalizeManualHdmiSourceCustom(L"　遊戲主機　") == std::optional<std::string>("遊戲主機"),
          "Unicode trim and UTF-8 persistence");
    Check(NormalizeManualHdmiSourceCustom(std::string(64, 'x')).has_value(), "64 characters accepted");
    Check(!NormalizeManualHdmiSourceCustom(std::string(65, 'x')), "65 characters rejected");
    for (const auto* value : {"line\nbreak", "line\rbreak", "tab\tname",
                             "control\x01", "delete\x7f", "c1\xc2\x85",
                             "separator\xe2\x80\xa8", "bad\xc0\xaf", "bad\xed\xa0\x80"})
        Check(!NormalizeManualHdmiSourceCustom(std::string_view(value)),
              "newline, controls, malformed UTF-8 rejected");
    Check(!NormalizeManualHdmiSourceCustom(std::string_view("A\0B", 3)), "embedded NUL rejected");
    std::string emoji;
    std::wstring wideEmoji;
    for (int i = 0; i < 64; ++i) {
        emoji += "\xf0\x9f\x8e\xae";
        wideEmoji += L"\xd83c\xdfae";
    }
    Check(NormalizeManualHdmiSourceCustom(emoji).has_value(), "64 UTF-8 scalars accepted");
    Check(NormalizeManualHdmiSourceCustom(std::wstring_view(wideEmoji)) ==
          std::optional<std::string>(emoji), "64 UTF-16 surrogate pairs accepted");
    Check(!NormalizeManualHdmiSourceCustom(std::wstring_view(L"\xd800")), "lone surrogate rejected");
    emoji += "\xf0\x9f\x8e\xae";
    Check(!NormalizeManualHdmiSourceCustom(emoji), "65 multibyte scalars rejected");
    Check(NormalizeManualHdmiSourceCustom("<img src=x onerror=alert(1)>").has_value(),
          "literal markup can be rendered safely as text");
}

void TestMessages() {
    for (const auto& [value, label] : kManualHdmiSources) {
        const std::wstring wide(value.begin(), value.end());
        const auto message = ParseSettingsMessage(
            L"{\"action\":\"setManualHdmiSource\",\"value\":\"" + wide + L"\"}");
        Check(message && message->text == wide, "canonical bridge payload preserved");
    }
    for (const auto* invalid : {
        LR"({"action":"setManualHdmiSource","value":"unknown"})",
        LR"({"action":"setManualHdmiSource","value":"Xbox"})",
        LR"({"action":"setManualHdmiSource","value":"xbox"})",
        LR"({"action":"setManualHdmiSource","value":"其他"})",
        LR"({"action":"setManualHdmiSource","value":true})",
        LR"({"action":"setManualHdmiSource","value":{"source":"ps5"}})",
        LR"({"action":"setManualHdmiSource"})",
        LR"({"action":"setManualHdmiSourceCustom","value":"A\nB"})",
        LR"({"action":"setManualHdmiSourceCustom","value":"A\u009fB"})"})
        Check(!ParseSettingsMessage(invalid), "invalid UI messages rejected");
    const auto custom = ParseSettingsMessage(
        LR"({"action":"setManualHdmiSourceCustom","value":"  Steam \"Deck\" \\ PC  "})");
    Check(custom && custom->text == L"Steam \"Deck\" \\ PC", "custom JSON escaped and trimmed");
    Check(ParseSettingsMessage(LR"({"action":"setManualHdmiSourceCustom","value":""})").has_value(),
          "empty custom bridge payload legal");
    Check(!ParseSettingsMessage(L"{\"action\":\"setManualHdmiSourceCustom\",\"value\":\"" +
          std::wstring(65, L'x') + L"\"}"), "overlength bridge custom rejected");
}

void TestConfig() {
    TemporaryConfig temporary;
    Config config;
    Check(config.manualHdmiSource == "auto" && config.manualHdmiSourceCustom.empty(),
          "missing config defaults");
    Check(config.Load(temporary.path.string()), "first launch creates config");
    Check(Read(temporary.path).find("manual_hdmi_source = auto\n") != std::string::npos,
          "first launch writes canonical schema");
    Check(config.SetManualHdmiSource("other") && config.SetManualHdmiSourceCustom(" Steam Deck "),
          "valid setters");
    {
        std::ofstream legacy(temporary.path, std::ios::trunc);
        legacy << "window_width = 1280\n";
    }
    Check(config.Load(temporary.path.string()) && config.manualHdmiSource == "auto" &&
          config.manualHdmiSourceCustom.empty(), "legacy/reused config defaults");
    {
        std::ofstream invalid(temporary.path, std::ios::trunc);
        invalid << "manual_hdmi_source = unsupported\nmanual_hdmi_source_custom = "
                << std::string(65, 'x') << '\n';
    }
    Check(config.Load(temporary.path.string()) && config.manualHdmiSource == "auto" &&
          config.manualHdmiSourceCustom.empty(), "invalid config fallback");
    for (const auto& [value, label] : kManualHdmiSources) {
        Check(config.SetManualHdmiSource(value), "set canonical config");
        Check(config.SetManualHdmiSourceCustom(L"  掌機 \"Deck\" \\ PC  "), "set Unicode custom");
        Check(config.Save(temporary.path.string()), "save canonical and custom");
        Config loaded;
        Check(loaded.Load(temporary.path.string()) && loaded.manualHdmiSource == value &&
              loaded.manualHdmiSourceCustom == "掌機 \"Deck\" \\ PC",
              "all canonical/custom config round-trips");
    }
    config.SetManualHdmiSource("switch2");
    config.SetManualHdmiSourceCustom("Steam Deck");
    Check(!config.SetManualHdmiSource("evil") && config.manualHdmiSource == "switch2",
          "invalid UI update preserves selected value");
    Check(!config.SetManualHdmiSourceCustom("Steam\nDeck") && config.manualHdmiSourceCustom == "Steam Deck",
          "invalid custom update preserves previous value");
    config.SetManualHdmiSource("auto");
    Check(config.manualHdmiSourceCustom == "Steam Deck" &&
          GetEffectiveHdmiSourceLabel(config.manualHdmiSource, L"automatic PS5") == L"automatic PS5",
          "manual to Auto restores detector and retains custom preference");
    config.SetManualHdmiSource("other");
    config.preferredDevice = L"Another capture card";
    Check(config.manualHdmiSource == "other" && config.manualHdmiSourceCustom == "Steam Deck",
          "identity is independent of capture selection");
    config.manualHdmiSource = "bad";
    config.manualHdmiSourceCustom = "bad\nhdr_enabled = true";
    Check(config.Save(temporary.path.string()), "save sanitizes corrupted in-memory identity");
    Config sanitized;
    Check(sanitized.Load(temporary.path.string()) && sanitized.manualHdmiSource == "auto" &&
          sanitized.manualHdmiSourceCustom.empty() && !sanitized.hdrEnabled,
          "unsafe identity cannot inject other settings");
}

void TestPolicyIsolation() {
    TemporaryConfig temporary;
    Config config;
    config.preferredDevice = L"GC553Pro";
    config.currentGameId = "spider-man-2";
    config.gameSettings[config.currentGameId] = {true, true};
    config.captureFormatOverrides[config.preferredDevice].format = L"P010";
    config.presentPacing = kPacingUnique;
    config.vsync = true;
    config.presentCapHz = 117;
    config.lowLatency = false;
    Check(config.Save(temporary.path.string()), "isolation baseline saved");
    const auto originalConfig = WithoutIdentity(Read(temporary.path));
    for (const auto& [value, label] : kManualHdmiSources) {
        Check(config.SetManualHdmiSource(value) && config.SetManualHdmiSourceCustom("Steam Deck"),
              "identity-only update");
        Check(config.Save(temporary.path.string()) &&
              WithoutIdentity(Read(temporary.path)) == originalConfig,
              "identity leaves every other serialized preference unchanged");
        for (auto actual : {NegotiatedCaptureFormatKind::Other, NegotiatedCaptureFormatKind::NV12,
                            NegotiatedCaptureFormatKind::P010})
        for (auto preference : {CaptureFormatPreference::Auto, CaptureFormatPreference::ManualNV12,
                                CaptureFormatPreference::ManualP010, CaptureFormatPreference::ManualOther})
        for (bool hdr : {false, true})
        for (bool automatic : {false, true})
        for (auto source : {Gc553ProSourceHdrState::Unknown, Gc553ProSourceHdrState::Sdr,
                            Gc553ProSourceHdrState::Hdr10Pq, Gc553ProSourceHdrState::OtherHdr}) {
            config.hdrEnabled = hdr;
            config.hdrAutoFromSource = automatic;
            const auto before = DecideGc553ProSourceOutputPolicy(
                actual, config.hdrEnabled, preference, config.hdrAutoFromSource, source);
            config.SetManualHdmiSource("auto");
            config.SetManualHdmiSource(value);
            GetEffectiveHdmiSourceLabel(value, L"automatic", L"SPD", L"Steam Deck");
            const auto after = DecideGc553ProSourceOutputPolicy(
                actual, config.hdrEnabled, preference, config.hdrAutoFromSource, source);
            Check(config.hdrEnabled == hdr && config.hdrAutoFromSource == automatic &&
                  before.desiredCaptureIsP010 == after.desiredCaptureIsP010 &&
                  before.reopenCapture == after.reopenCapture && before.hdrRejected == after.hdrRejected,
                  "manual identity never changes HDR, P010, or reopen decision");
        }
        config.hdrEnabled = false;
        config.hdrAutoFromSource = true;
    }
}

void TestActivityStartTime() {
    using Clock = std::chrono::system_clock;
    const auto started = Clock::time_point{std::chrono::seconds(1700000000)};
    const auto later = started + std::chrono::minutes(5);
    auto activityStart = HdmiSourceActivityStartTime({}, false, started);
    Check(activityStart == started, "first activity records start time");
    for (const auto& [value, label] : kManualHdmiSources) {
        const auto identity = GetEffectiveHdmiSourceLabel(value, L"Detected PS5", L"SPD");
        Check(!identity.empty(), "presence uses shared manual/automatic identity");
        activityStart = HdmiSourceActivityStartTime(activityStart, true, later);
        Check(activityStart == started, "identity refresh cannot restart Discord activity");
    }
    Check(HdmiSourceActivityStartTime({}, true, later) == later,
          "first identity refresh initializes a previously absent activity");
    Check(HdmiSourceActivityStartTime(started, false, later) == later,
          "explicit game selection starts a new activity");
    Check(HdmiSourceActivityStartTime(started, true, Clock::time_point{}) == started,
          "display refresh retains the previous start independently of refresh time");
}
} // namespace

int main() {
    try {
        TestLabels();
        TestCustomValidation();
        TestMessages();
        TestConfig();
        TestPolicyIsolation();
        TestActivityStartTime();
        std::cout << checks << " manual HDMI source checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
