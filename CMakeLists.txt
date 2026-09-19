cmake_minimum_required(VERSION 3.20)
project(NitLink VERSION 1.2.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows-specific settings
if(WIN32)
    add_definitions(-DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN)
endif()

# Force MSVC to treat source files as UTF-8. Without this flag, MSVC reads
# files in the system's default codepage (usually Windows-1252) and chokes
# on any byte sequence >= 0x80, even in string literals or comments.
if(MSVC)
    add_compile_options(/utf-8)
endif()

# Optional: nlohmann_json if installed via vcpkg. Falls back to bundled header.
find_package(nlohmann_json CONFIG QUIET)

set(NITLINK_SOURCES
    src/main.cpp
    src/app/application.cpp
    src/app/application.h
    src/app/config.cpp
    src/app/config.h
    src/app/localization.cpp
    src/app/localization.h
    src/app/game_database.cpp
    src/app/game_database.h
    src/app/source_cadence.h
    src/app/WebViewSettings.cpp
    src/app/WebViewSettings.h
    src/capture/capture_device.cpp
    src/capture/capture_device.h
    src/capture/dshow_capture.cpp
    src/capture/dshow_capture.h
    src/capture/device_enumerator.cpp
    src/capture/device_enumerator.h
    src/capture/elgato_hdr_control.cpp
    src/capture/elgato_hdr_control.h
    src/capture/elgato_hid_4ks.cpp
    src/capture/elgato_hid_4ks.h
    src/capture/frame_buffer.cpp
    src/capture/frame_buffer.h
    src/capture/frame_differ.cpp
    src/capture/frame_differ.h
    src/capture/hdr_source_poller.cpp
    src/capture/hdr_source_poller.h
    src/capture/placeholder_detector.cpp
    src/capture/placeholder_detector.h
    src/capture/p010_format_selector.cpp
    src/capture/p010_format_selector.h
    src/renderer/dx11_renderer.cpp
    src/renderer/dx11_renderer.h
    src/renderer/hdr_tone_map.h
    src/renderer/hdr_tone_map_hlsl.h
    src/renderer/window.cpp
    src/renderer/window.h
    src/input/hotkey_manager.cpp
    src/input/hotkey_manager.h
    src/audio/audio_router.cpp
    src/audio/audio_router.h
    src/overlay/overlay.cpp
    src/overlay/overlay.h
    src/upscale/nis_upscaler.cpp
    src/upscale/nis_upscaler.h
    src/discord/discord_rpc.cpp
    src/discord/discord_rpc.h
    src/app.rc
)

add_executable(NitLink WIN32 ${NITLINK_SOURCES})

target_link_libraries(NitLink PRIVATE
    d3d11
    d2d1
    dwrite
    dxgi
    d3dcompiler
    dxguid
    mf
    mfplat
    mfreadwrite
    mfuuid
    ole32
    oleaut32
    strmiids
    uuid
    winmm
    shlwapi
    shcore
    avrt
    propsys
    dwmapi
    windowscodecs
    # hid + setupapi: Windows HID Class API for the Elgato 4K S vendor
    # collection on USB interface 7 (src/capture/elgato_hid_4ks.cpp).
    # 4K S does not expose its HDR tonemap toggle through IKsPropertySet
    # like the 4K Pro does; we send a 255-byte HID Output Report instead.
    hid
    setupapi
    ${CMAKE_SOURCE_DIR}/third_party/webview2/lib/x64/WebView2LoaderStatic.lib
)

if(nlohmann_json_FOUND)
    target_link_libraries(NitLink PRIVATE nlohmann_json::nlohmann_json)
else()
    message(STATUS "nlohmann_json not found - using bundled header")
endif()

target_include_directories(NitLink PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/third_party/wil/include
    ${CMAKE_SOURCE_DIR}/third_party/webview2/include
)

install(TARGETS NitLink RUNTIME DESTINATION bin)
install(FILES nitlink-menu.html DESTINATION bin)
install(DIRECTORY locales DESTINATION bin)
install(FILES third_party/nis/NIS_Scaler.h third_party/nis/LICENSE.txt
        DESTINATION bin/third_party/nis)
install(FILES LICENSE LICENSES.md ACKNOWLEDGMENTS.md DESTINATION bin)

# nis_upscaler.cpp compiles NIS_Main.hlsl at runtime via D3DCompile, which
# #includes "NIS_Scaler.h" from disk. Copy it next to the .exe so include
# resolution works regardless of working directory.
add_custom_command(TARGET NitLink POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:NitLink>/third_party/nis
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_SOURCE_DIR}/third_party/nis/NIS_Scaler.h
        $<TARGET_FILE_DIR:NitLink>/third_party/nis/NIS_Scaler.h
)

# Keep the WebView2 settings HTML and external localization resources in sync
# even when NitLink.exe itself does not need to relink. This target intentionally
# has no output: copy_if_different keeps unchanged files' timestamps stable,
# while the dependency makes a direct NitLink target build refresh resources.
add_custom_target(NitLinkRuntimeResources
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:NitLink>/locales
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/nitlink-menu.html
        $<TARGET_FILE_DIR:NitLink>/nitlink-menu.html
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/locales/en-US.js
        $<TARGET_FILE_DIR:NitLink>/locales/en-US.js
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/locales/zh-TW.js
        $<TARGET_FILE_DIR:NitLink>/locales/zh-TW.js
)
add_dependencies(NitLink NitLinkRuntimeResources)

include(CTest)
if(BUILD_TESTING)
    add_executable(NitLinkToneMapTests
        tests/hdr_tone_map_reference_tests.cpp
        src/renderer/hdr_tone_map.h
        src/renderer/hdr_tone_map_hlsl.h
    )
    target_include_directories(NitLinkToneMapTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(NitLinkToneMapTests PRIVATE
        d3d11
        d3dcompiler
    )
    add_test(NAME hdr_tone_map_reference COMMAND NitLinkToneMapTests)

    add_executable(NitLinkP010SelectorTests
        tests/p010_format_selector_tests.cpp
        src/capture/p010_format_selector.cpp
        src/capture/p010_format_selector.h
    )
    target_include_directories(NitLinkP010SelectorTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    add_test(NAME p010_format_selector COMMAND NitLinkP010SelectorTests)
endif()
