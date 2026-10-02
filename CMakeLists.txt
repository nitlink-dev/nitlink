cmake_minimum_required(VERSION 3.20)
project(NitLink VERSION 1.2.3 LANGUAGES CXX)

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
    add_compile_options(/utf-8 /GS /guard:cf)
    add_link_options(/guard:cf /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /CETCOMPAT)
endif()

set(NITLINK_SOURCES
    src/main.cpp
    src/app/application.cpp
    src/app/application.h
    src/app/config.cpp
    src/app/config.h
    src/app/hdmi_source.h
    src/app/hdmi_source_display.h
    src/app/localization.cpp
    src/app/localization.h
    src/app/game_database.cpp
    src/app/game_database.h
    src/app/source_cadence.h
    src/app/presentation_state.h
    src/app/capture_output_policy.h
    src/app/WebViewSettings.cpp
    src/app/WebViewSettings.h
    src/app/settings_message.cpp
    src/app/settings_message.h
    src/app/webview_policy.h
    src/capture/capture_device.cpp
    src/capture/capture_device.h
    src/capture/dshow_capture.cpp
    src/capture/dshow_capture.h
    src/capture/device_enumerator.cpp
    src/capture/device_enumerator.h
    src/capture/elgato_hdr_control.cpp
    src/capture/elgato_hdr_control.h
    src/capture/gc553pro_hdr_source.cpp
    src/capture/gc553pro_hdr_source.h
    src/capture/hdmi_raw_timing.h
    src/capture/hdmi_source_vrr.h
    src/capture/gc553pro_vtem.h
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
    src/renderer/playback_power.h
    src/input/hotkey_manager.cpp
    src/input/hotkey_manager.h
    src/audio/audio_router.cpp
    src/audio/audio_router.h
    src/overlay/no_signal_image.cpp
    src/overlay/overlay.cpp
    src/overlay/overlay.h
    src/overlay/no_signal_cache.h
    src/overlay/no_signal_layout.cpp
    src/overlay/no_signal_layout.h
    src/upscale/nis_upscaler.cpp
    src/upscale/nis_upscaler.h
    src/discord/pipe_transport.cpp
    src/discord/discord_rpc.cpp
    src/discord/discord_rpc.h
    src/app.rc
)

add_executable(NitLink WIN32 ${NITLINK_SOURCES})

# Keep WIL line diagnostics without embedding source filenames in the executable.
target_compile_definitions(NitLink PRIVATE RESULT_DIAGNOSTICS_LEVEL=2)

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

target_include_directories(NitLink PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/third_party/wil/include
    ${CMAKE_SOURCE_DIR}/third_party/webview2/include
)

install(TARGETS NitLink RUNTIME DESTINATION bin)
install(FILES nitlink-menu.html DESTINATION bin)
install(DIRECTORY locales DESTINATION bin)
install(DIRECTORY assets/menu DESTINATION bin/assets)
install(FILES third_party/nis/NIS_Scaler.h third_party/nis/LICENSE.txt
        DESTINATION bin/third_party/nis)
install(FILES LICENSE LICENSES.md ACKNOWLEDGMENTS.md DESTINATION bin)
install(FILES docs/hdmi-source-metadata.md DESTINATION bin/docs)

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
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets/menu
        $<TARGET_FILE_DIR:NitLink>/assets/menu
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
    add_executable(NitLinkNativeSecurityTests tests/native_security_tests.cpp
        src/app/config.cpp src/capture/frame_buffer.cpp src/overlay/no_signal_image.cpp)
    target_include_directories(NitLinkNativeSecurityTests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(NitLinkNativeSecurityTests PRIVATE ole32 windowscodecs shlwapi)
    add_test(NAME native_input_security COMMAND NitLinkNativeSecurityTests)

    add_executable(NitLinkPipeSecurityTests tests/pipe_security_tests.cpp src/discord/pipe_transport.cpp src/discord/discord_rpc.cpp)
    target_include_directories(NitLinkPipeSecurityTests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(NitLinkPipeSecurityTests PRIVATE advapi32)
    add_test(NAME pipe_security COMMAND NitLinkPipeSecurityTests)

    add_executable(NitLinkMediaSecurityTests tests/media_security_tests.cpp)
    target_include_directories(NitLinkMediaSecurityTests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(NitLinkMediaSecurityTests PRIVATE ole32 oleaut32 strmiids uuid mfuuid avrt)
    add_test(NAME media_metadata_security COMMAND NitLinkMediaSecurityTests)

    add_executable(NitLinkShaderSecurityTests tests/shader_security_tests.cpp)
    target_include_directories(NitLinkShaderSecurityTests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(NitLinkShaderSecurityTests PRIVATE d3d11 d3dcompiler)
    add_dependencies(NitLinkShaderSecurityTests NitLink)
    add_test(NAME packaged_shader_security COMMAND NitLinkShaderSecurityTests)
    set_tests_properties(packaged_shader_security PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests")

    add_executable(NitLinkSettingsSecurityTests
        src/app/localization.cpp
        tests/settings_security_tests.cpp
        src/app/settings_message.cpp
        src/app/game_database.cpp
    )
    target_include_directories(NitLinkSettingsSecurityTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party/webview2/include)
    add_test(NAME settings_security COMMAND NitLinkSettingsSecurityTests)

    option(NITLINK_TEST_WEBVIEW_RUNTIME "Run the isolated, software-rendered WebView2 host test" ON)
    if(NITLINK_TEST_WEBVIEW_RUNTIME)
        add_executable(NitLinkWebViewRuntimeTests
            tests/webview_runtime_tests.cpp
            src/app/WebViewSettings.cpp
            src/app/settings_message.cpp
            src/app/game_database.cpp
        )
        target_include_directories(NitLinkWebViewRuntimeTests PRIVATE
            ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party
            ${CMAKE_SOURCE_DIR}/third_party/webview2/include)
        target_link_libraries(NitLinkWebViewRuntimeTests PRIVATE ole32 oleaut32 shlwapi
            ${CMAKE_SOURCE_DIR}/third_party/webview2/lib/x64/WebView2LoaderStatic.lib)
        add_dependencies(NitLinkWebViewRuntimeTests NitLinkRuntimeResources)
        add_test(NAME webview_runtime_security COMMAND NitLinkWebViewRuntimeTests)
        set_tests_properties(webview_runtime_security PROPERTIES TIMEOUT 60)
        add_test(NAME webview_missing_assets
            COMMAND ${CMAKE_COMMAND}
                "-DTEST_EXE=$<TARGET_FILE:NitLinkWebViewRuntimeTests>"
                "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/webview-missing-assets"
                -P "${CMAKE_SOURCE_DIR}/tests/webview_missing_assets.cmake")
        set_tests_properties(webview_missing_assets PROPERTIES TIMEOUT 60)
    endif()

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

    add_executable(NitLinkPlaceholderTests
        tests/placeholder_detector_tests.cpp
        src/capture/placeholder_detector.cpp
        src/capture/placeholder_detector.h
    )
    target_include_directories(NitLinkPlaceholderTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    add_test(NAME placeholder_detector COMMAND NitLinkPlaceholderTests)

    add_executable(NitLinkNoSignalLayoutTests
        tests/no_signal_layout_tests.cpp
        src/overlay/no_signal_layout.cpp
        src/overlay/no_signal_layout.h
    )
    target_include_directories(NitLinkNoSignalLayoutTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    add_test(NAME no_signal_layout COMMAND NitLinkNoSignalLayoutTests)

    add_executable(NitLinkNoSignalCacheTests
        tests/no_signal_cache_tests.cpp
        src/overlay/no_signal_cache.h
    )
    target_include_directories(NitLinkNoSignalCacheTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    add_test(NAME no_signal_cache COMMAND NitLinkNoSignalCacheTests)

    add_executable(NitLinkPresentationStateTests
        tests/presentation_state_tests.cpp
        src/app/presentation_state.h
        src/capture/placeholder_detector.cpp
        src/capture/placeholder_detector.h
    )
    target_include_directories(NitLinkPresentationStateTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    add_test(NAME presentation_state COMMAND NitLinkPresentationStateTests)

    add_executable(NitLinkSignalResyncTests tests/signal_resync_presentation_tests.cpp
        src/capture/frame_buffer.cpp src/capture/placeholder_detector.cpp
        src/capture/gc553pro_hdr_source.cpp src/app/localization.cpp)
    target_include_directories(NitLinkSignalResyncTests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(NitLinkSignalResyncTests PRIVATE ole32 oleaut32 strmiids)
    add_test(NAME signal_resync_presentation COMMAND NitLinkSignalResyncTests)

    add_executable(NitLinkP010NegotiationTests
        tests/p010_negotiation_tests.cpp
        src/capture/capture_device.cpp
        src/capture/dshow_capture.cpp
        src/capture/p010_format_selector.cpp
    )
    target_include_directories(NitLinkP010NegotiationTests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(NitLinkP010NegotiationTests PRIVATE
        mf mfplat mfreadwrite mfuuid ole32 oleaut32 strmiids uuid avrt
    )
    add_test(NAME p010_native_negotiation COMMAND NitLinkP010NegotiationTests)

    add_executable(NitLinkConfigRateTests
        tests/config_rate_tests.cpp
        src/app/config.cpp
    )
    target_include_directories(NitLinkConfigRateTests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME config_rate_migration COMMAND NitLinkConfigRateTests)

    add_executable(NitLinkWindowSizeTests
        tests/window_size_tests.cpp
        src/renderer/window.cpp
        src/app/config.cpp
    )
    target_include_directories(NitLinkWindowSizeTests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(NitLinkWindowSizeTests PRIVATE user32 gdi32 dwmapi)
    add_test(NAME window_size_persistence COMMAND NitLinkWindowSizeTests)

    add_executable(NitLinkPlaybackPowerTests
        tests/playback_power_tests.cpp
        src/renderer/window.cpp
        src/app/config.cpp
    )
    target_include_directories(NitLinkPlaybackPowerTests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(NitLinkPlaybackPowerTests PRIVATE user32 gdi32 dwmapi)
    add_test(NAME playback_power COMMAND NitLinkPlaybackPowerTests)

    add_executable(NitLinkCaptureOutputPolicyTests
        tests/capture_output_policy_tests.cpp
        src/app/capture_output_policy.h
    )
    target_include_directories(NitLinkCaptureOutputPolicyTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    add_test(NAME capture_output_policy COMMAND NitLinkCaptureOutputPolicyTests)

    add_executable(NitLinkVSyncConfigTests
        tests/vsync_config_tests.cpp
        src/app/config.cpp
    )
    target_include_directories(NitLinkVSyncConfigTests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME vsync_config_persistence COMMAND NitLinkVSyncConfigTests)

    add_executable(NitLinkConfigTests
        tests/config_tests.cpp
        src/app/config.cpp
        src/app/config.h
    )
    target_include_directories(NitLinkConfigTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    add_test(NAME config_round_trip COMMAND NitLinkConfigTests)

    add_executable(NitLinkCaptureDeviceNamesTests
        tests/capture_device_names_tests.cpp
        src/app/capture_device_names.h
    )
    target_include_directories(NitLinkCaptureDeviceNamesTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    add_test(NAME capture_device_names_cache COMMAND NitLinkCaptureDeviceNamesTests)

    add_executable(NitLinkFrameBufferActivityTests
        tests/frame_buffer_activity_tests.cpp
        src/capture/frame_buffer.cpp
        src/capture/frame_buffer.h
    )
    target_include_directories(NitLinkFrameBufferActivityTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    add_test(NAME frame_buffer_activity COMMAND NitLinkFrameBufferActivityTests)

    add_executable(NitLinkGc553ProHdrTests
        tests/gc553pro_hdr_source_tests.cpp
        src/capture/gc553pro_hdr_source.cpp
    )
    target_include_directories(NitLinkGc553ProHdrTests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(NitLinkGc553ProHdrTests PRIVATE ole32 oleaut32 strmiids)
    add_test(NAME gc553pro_hdr_source COMMAND NitLinkGc553ProHdrTests)

    add_executable(NitLinkManualHdmiSourceTests tests/manual_hdmi_source_tests.cpp
        src/app/config.cpp src/app/settings_message.cpp src/app/game_database.cpp)
    target_include_directories(NitLinkManualHdmiSourceTests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME manual_hdmi_source COMMAND NitLinkManualHdmiSourceTests)

    add_executable(NitLinkGc553ProRawTimingTests tests/gc553pro_raw_timing_tests.cpp)
    target_include_directories(NitLinkGc553ProRawTimingTests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME gc553pro_raw_timing COMMAND NitLinkGc553ProRawTimingTests)

    add_executable(NitLinkGc553ProSourceVrrTests tests/gc553pro_source_vrr_tests.cpp)
    target_include_directories(NitLinkGc553ProSourceVrrTests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME gc553pro_source_vrr COMMAND NitLinkGc553ProSourceVrrTests)

    add_executable(NitLinkGc553ProSourceMetadataTests tests/gc553pro_source_metadata_poller_tests.cpp
        src/capture/hdr_source_poller.cpp src/capture/gc553pro_hdr_source.cpp
        src/capture/elgato_hdr_control.cpp)
    target_include_directories(NitLinkGc553ProSourceMetadataTests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(NitLinkGc553ProSourceMetadataTests PRIVATE ole32 oleaut32 strmiids)
    add_test(NAME gc553pro_source_metadata_poller COMMAND NitLinkGc553ProSourceMetadataTests)
    set_tests_properties(gc553pro_source_metadata_poller PROPERTIES TIMEOUT 30)
endif()
