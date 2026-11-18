#include "app/config.h"

#include <filesystem>
#include <fstream>
#include <iostream>

int main()
{
    const auto path = std::filesystem::temp_directory_path() /
        "nitlink-custom-no-signal-config-test.txt";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    // A pre-E1 config with none of the new keys keeps compiled defaults.
    {
        std::ofstream legacy(path, std::ios::trunc);
        legacy << "window_width = 1280\n";
    }
    NitLink::Config legacyLoaded;
    if (!legacyLoaded.Load(path.string())) return 1;
    if (legacyLoaded.audioFifoMs != 40 || legacyLoaded.audioRenderMs != 30 || !legacyLoaded.audioDrift || legacyLoaded.audioExclusive) return 19;
    if (legacyLoaded.noSignalMode != "default" ||
        !legacyLoaded.noSignalImage.empty() ||
        legacyLoaded.noSignalFit != "contain" ||
        !legacyLoaded.noSignalDimImage) return 2;

    NitLink::Config defaults;
    if (!defaults.Save(path.string())) return 3;
    NitLink::Config defaultLoaded;
    if (!defaultLoaded.Load(path.string())) return 4;
    if (defaultLoaded.noSignalMode != "default" ||
        !defaultLoaded.noSignalImage.empty() ||
        defaultLoaded.noSignalFit != "contain" ||
        !defaultLoaded.noSignalDimImage) return 5;

    NitLink::Config saved;
    saved.audioFifoMs = 7; saved.audioRenderMs = 9; saved.audioDrift = false; saved.audioExclusive = true;
    saved.noSignalMode = "image";
    saved.noSignalImage =
        "C:\\Users\\測試者\\Pictures\\無訊號圖片.png";
    saved.noSignalFit = "cover";
    saved.noSignalDimImage = false;
    if (!saved.Save(path.string())) return 6;

    NitLink::Config loaded;
    if (!loaded.Load(path.string())) return 7;
    if (loaded.audioFifoMs != 7 || loaded.audioRenderMs != 9 || loaded.audioDrift || !loaded.audioExclusive) return 18;
    if (loaded.noSignalMode != saved.noSignalMode ||
        loaded.noSignalImage != saved.noSignalImage ||
        loaded.noSignalFit != saved.noSignalFit ||
        loaded.noSignalDimImage != saved.noSignalDimImage) return 8;

    saved.noSignalFit = "stretch";
    saved.noSignalDimImage = true;
    if (!saved.Save(path.string())) return 9;
    NitLink::Config stretchLoaded;
    if (!stretchLoaded.Load(path.string()) ||
        stretchLoaded.noSignalFit != "stretch" ||
        !stretchLoaded.noSignalDimImage) return 10;

    // Switching back to the branded page preserves the selected image path.
    // Only the explicit remove-image action clears it.
    saved.noSignalMode = "default";
    if (!saved.Save(path.string())) return 11;
    NitLink::Config defaultWithImageLoaded;
    if (!defaultWithImageLoaded.Load(path.string()) ||
        defaultWithImageLoaded.noSignalMode != "default" ||
        defaultWithImageLoaded.noSignalImage != saved.noSignalImage) return 12;

    saved.noSignalImage.clear();
    if (!saved.Save(path.string())) return 13;
    NitLink::Config clearedLoaded;
    if (!clearedLoaded.Load(path.string()) ||
        clearedLoaded.noSignalMode != "default" ||
        !clearedLoaded.noSignalImage.empty()) return 14;

    {
        std::ofstream invalid(path, std::ios::trunc);
        invalid << "no_signal_mode = unsupported\n"
                << "no_signal_fit = unsupported\n"
                << "no_signal_image = C:\\Users\\測試者\\無訊號.bmp\n";
    }
    NitLink::Config invalidLoaded;
    if (!invalidLoaded.Load(path.string())) return 15;
    if (invalidLoaded.noSignalMode != "default" ||
        invalidLoaded.noSignalFit != "contain") return 16;

    // The card-side manual format YUY2 survives a save/load.
    {
        NitLink::Config formats;
        formats.captureFormatOverrides[L"USB Capture A"] = {1920, 1080, 60, L"YUY2", 60, 1};
        if (!formats.Save(path.string())) return 20;
        NitLink::Config formatsLoaded;
        if (!formatsLoaded.Load(path.string()) ||
            formatsLoaded.GetOverride(L"USB Capture A").format != L"YUY2" ||
            formatsLoaded.GetOverride(L"USB Capture A").fpsNumerator != 60) return 21;
    }

    if (!std::filesystem::remove(path, ec) && std::filesystem::exists(path)) {
        return 17;
    }

    std::cout << "custom No Signal config tests passed\n";
    return 0;
}
