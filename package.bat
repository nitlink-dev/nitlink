@echo off
REM ============================================================================
REM package.bat: Build a NitLink release zip for distribution / testing.
REM
REM Usage:  double-click, or run from cmd in the repo root.
REM
REM Assumes a Release build has already been produced at:
REM  out\build\x64-Release\NitLink.exe
REM
REM Output: NitLink-<version>-win64.zip in the repo root.
REM ============================================================================

setlocal EnableDelayedExpansion
cd /d "%~dp0"
if errorlevel 1 (
  echo [ERROR] Could not open the package source directory.
  pause
  exit /b 1
)

REM --- Configuration ---------------------------------------------------------
set VERSION=1.2.4
set BUILD_DIR=out\build\x64-Release
set BUILD_DIR_ALT=build\Release
set STAGING_DIR=NitLink-%VERSION%-win64
set OUTPUT_ZIP=NitLink-%VERSION%-win64.zip

echo.
echo ==========================================================================
echo  NitLink packager v%VERSION%
echo ==========================================================================
echo.

REM --- Sanity check: did the build actually happen? --------------------------
REM Prefer the Visual Studio CMakeSettings.json layout (out\build\x64-Release),
REM fall back to the plain "cmake -B build" layout (build\Release).
if not exist "%BUILD_DIR%\NitLink.exe" (
  if exist "%BUILD_DIR_ALT%\NitLink.exe" (
    echo NitLink.exe not found at %BUILD_DIR%, using %BUILD_DIR_ALT% instead.
    set BUILD_DIR=%BUILD_DIR_ALT%
    ) else (
    echo [ERROR] NitLink.exe not found at either:
    echo  %BUILD_DIR%\NitLink.exe
    echo  %BUILD_DIR_ALT%\NitLink.exe
    echo.
    echo Build the x64-Release configuration first ^(Visual Studio: Build menu,
    echo or cmake --build build --config Release^), then re-run this script.
    echo.
    pause
    exit /b 1
  )
)

REM --- Clean any previous staging --------------------------------------------
if exist "%STAGING_DIR%" (
  echo Cleaning previous staging directory...
  rmdir /s /q "%STAGING_DIR%"
)
if exist "%OUTPUT_ZIP%" (
  echo Removing previous zip...
  del /q "%OUTPUT_ZIP%"
)

REM --- Create staging tree ---------------------------------------------------
echo Creating staging directory: %STAGING_DIR%
mkdir "%STAGING_DIR%"
mkdir "%STAGING_DIR%\third_party\nis"
mkdir "%STAGING_DIR%\docs"
mkdir "%STAGING_DIR%\locales"
mkdir "%STAGING_DIR%\assets\menu"

REM --- Copy required files ---------------------------------------------------
echo Copying NitLink.exe...
copy /y "%BUILD_DIR%\NitLink.exe" "%STAGING_DIR%\" >nul
if errorlevel 1 (
  echo [ERROR] Failed to copy NitLink.exe
  pause
  exit /b 1
)

echo Copying nitlink-menu.html...
copy /y "%BUILD_DIR%\nitlink-menu.html" "%STAGING_DIR%\" >nul
if errorlevel 1 (
  echo [WARN] nitlink-menu.html missing from build dir, trying repo root...
  copy /y "nitlink-menu.html" "%STAGING_DIR%\" >nul
  if errorlevel 1 (
    echo [ERROR] nitlink-menu.html not found anywhere; settings menu will not work.
    pause
    exit /b 1
  )
)

echo Copying localization resources...
copy /y "%BUILD_DIR%\locales\en-US.js" "%STAGING_DIR%\locales\" >nul
if errorlevel 1 (
  echo [ERROR] en-US localization resource is missing from the build output.
  pause
  exit /b 1
)
copy /y "%BUILD_DIR%\locales\zh-TW.js" "%STAGING_DIR%\locales\" >nul
if errorlevel 1 (
  echo [ERROR] zh-TW localization resource is missing from the build output.
  pause
  exit /b 1
)
copy /y "%BUILD_DIR%\locales\zh-CN.js" "%STAGING_DIR%\locales\" >nul
if errorlevel 1 (
  echo [ERROR] zh-CN localization resource is missing from the build output.
  pause
  exit /b 1
)
copy /y "%BUILD_DIR%\locales\de-DE.js" "%STAGING_DIR%\locales\" >nul
if errorlevel 1 (
  echo [ERROR] de-DE localization resource is missing from the build output.
  pause
  exit /b 1
)
copy /y "%BUILD_DIR%\locales\ko-KR.js" "%STAGING_DIR%\locales\" >nul
if errorlevel 1 (
  echo [ERROR] ko-KR localization resource is missing from the build output.
  pause
  exit /b 1
)

echo Copying settings assets...
for %%F in (menu.js Archivo-400.ttf Archivo-500.ttf Archivo-600.ttf Archivo-OFL.txt kofi-cup.png) do (
  copy /y "%BUILD_DIR%\assets\menu\%%F" "%STAGING_DIR%\assets\menu\" >nul
  if errorlevel 1 (
    echo [ERROR] Required settings asset %%F is missing from the build output.
    pause
    exit /b 1
  )
)

echo Copying NIS shader header...
copy /y "%BUILD_DIR%\third_party\nis\NIS_Scaler.h" "%STAGING_DIR%\third_party\nis\" >nul
if errorlevel 1 (
  echo [WARN] NIS_Scaler.h missing from build dir, trying repo path...
  copy /y "third_party\nis\NIS_Scaler.h" "%STAGING_DIR%\third_party\nis\" >nul
  if errorlevel 1 (
    echo [ERROR] NIS_Scaler.h not found; NIS upscaling will fail at runtime.
    pause
    exit /b 1
  )
)

REM --- License + attribution files (required for redistribution) ------------
echo Copying LICENSE (project MIT)...
copy /y "LICENSE" "%STAGING_DIR%\" >nul
if errorlevel 1 (
  echo [ERROR] LICENSE file not found at repo root.
  pause
  exit /b 1
)

echo Copying LICENSES.md (third-party notices)...
copy /y "LICENSES.md" "%STAGING_DIR%\" >nul
if errorlevel 1 (
  echo [ERROR] LICENSES.md not found at repo root.
  pause
  exit /b 1
)

echo Copying ACKNOWLEDGMENTS.md (non-bundled research references)...
copy /y "ACKNOWLEDGMENTS.md" "%STAGING_DIR%\" >nul
if errorlevel 1 (
  echo [ERROR] ACKNOWLEDGMENTS.md not found at repo root.
  pause
  exit /b 1
)

echo Copying docs\4ks-hdr-tonemap.md (referenced by ACKNOWLEDGMENTS.md)...
copy /y "docs\4ks-hdr-tonemap.md" "%STAGING_DIR%\docs\" >nul
if errorlevel 1 (
  echo [ERROR] docs\4ks-hdr-tonemap.md not found at repo root.
  pause
  exit /b 1
)

echo Copying NIS LICENSE.txt...
copy /y "third_party\nis\LICENSE.txt" "%STAGING_DIR%\third_party\nis\" >nul
if errorlevel 1 (
  echo [ERROR] third_party\nis\LICENSE.txt not found.
  pause
  exit /b 1
)

echo Copying release notes...
copy /y "release-notes-%VERSION%.md" "%STAGING_DIR%\RELEASE-NOTES.md" >nul
if errorlevel 1 (
  echo [ERROR] Release notes are missing.
  pause
  exit /b 1
)

REM --- Optionally copy any MSVC runtime DLLs that landed next to the .exe ----
REM  If the project is built with /MD (default), Visual Studio sometimes
REM  stages vcruntime140.dll and msvcp140.dll next to the .exe. If they're
REM  there, ship them. If not, users need the VC++ redistributable installed.
for %%F in (vcruntime140.dll vcruntime140_1.dll msvcp140.dll msvcp140_1.dll msvcp140_2.dll) do (
  if exist "%BUILD_DIR%\%%F" (
    echo Copying runtime: %%F
    copy /y "%BUILD_DIR%\%%F" "%STAGING_DIR%\" >nul
  )
)

REM --- README for testers ----------------------------------------------------
echo Writing README.txt...
(
echo NitLink v%VERSION%
echo =================
echo.
echo Low-latency 4K HDR capture card viewer for Windows.
echo Built and tested against the Elgato 4K Pro, 4K S, and 4K X.
echo.
echo HOW TO RUN
echo ----------
echo 1. Extract this folder anywhere ^(Desktop is fine^).
echo 2. Double-click NitLink.exe.
echo.
echo Windows SmartScreen may warn you because the binary is not yet
echo code-signed. Verify the source of the download before launching.
echo.
echo REQUIREMENTS
echo ------------
echo - Windows 10 ^(1809 or later^) or Windows 11
echo - DirectX 11 capable GPU
echo - Current Microsoft Edge WebView2 Evergreen runtime ^(preinstalled on Windows 11^)
echo   Update the runtime if NitLink reports missing security features.
echo - Microsoft Visual C++ runtime ^(included DLLs or latest x64 Redistributable^)
echo  https://aka.ms/vc14/vc_redist.x64.exe
echo - A capture card ^(Elgato 4K Pro/X recommended^) for live preview.
echo  Without a card, the app will show a "no capture device" message
echo  and exit; that is expected behavior.
echo.
echo HOTKEYS
echo -------
echo F1  Open / close settings menu
echo Alt+H  Toggle HDR
echo Alt+R  Cycle color-range override ^(Auto / Full / Limited^)
echo Alt+A  Cycle aspect ratio ^(Auto / 4:3 / 16:9 / 16:10 / 21:9 / Stretch^)
echo Alt+L  Toggle low-latency mode
echo Alt+V  Toggle VSync
echo F11 or Alt+Enter  Toggle fullscreen
echo Alt+O  Toggle picture-in-picture
echo Ctrl+F3  Toggle HUD overlay
echo Ctrl+S  Save screenshot to Pictures\NitLink\
echo.
echo CONFIG
echo ------
echo Settings are stored in nitlink.json next to the executable, created
echo automatically on first launch.
echo.
echo CREDITS
echo -------
echo See LICENSES.md for third-party software bundled or linked in this
echo build, and ACKNOWLEDGMENTS.md for external research / protocol
echo references that informed development but are not bundled.
echo.
echo SUPPORT
echo -------
echo Bug reports: https://github.com/nitlink-dev/nitlink/issues
) > "%STAGING_DIR%\README.txt"

REM Validate the exact file set and any redistributed Microsoft runtime DLLs.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\validate-package.ps1" -Path "%STAGING_DIR%"
if errorlevel 1 (
  echo [ERROR] Package validation failed.
  pause
  exit /b 1
)

REM --- Build the zip ---------------------------------------------------------
REM  PowerShell's Compress-Archive ships with every Windows 10 / 11.
echo Creating zip: %OUTPUT_ZIP%
powershell -NoProfile -Command "Compress-Archive -Path '%STAGING_DIR%' -DestinationPath '%OUTPUT_ZIP%' -Force"
if errorlevel 1 (
  echo [ERROR] Failed to create zip
  pause
  exit /b 1
)

REM --- Report ----------------------------------------------------------------
echo.
echo ==========================================================================
echo  DONE
echo ==========================================================================
echo.
echo Package contents:
dir /b "%STAGING_DIR%"
echo.
for %%F in ("%OUTPUT_ZIP%") do (
  set /a SIZE_KB=%%~zF / 1024
  echo Output: %OUTPUT_ZIP%  ^(!SIZE_KB! KB^)
)
echo.
echo Ready to copy onto another machine. Test by extracting and double-clicking
echo NitLink.exe. Without a capture card, expect the "no capture device" error
echo message; that confirms the binary loaded and ran.
echo.

REM --- Optional: leave staging dir in place for inspection -------------------
REM  If you'd rather it cleans up automatically, uncomment the next line:
REM rmdir /s /q "%STAGING_DIR%"

pause
endlocal
