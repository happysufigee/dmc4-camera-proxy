@echo off
REM Build script for DMC4 Camera Proxy
REM Run this from a Visual Studio Developer Command Prompt

echo Building DMC4 Camera Proxy...

REM Check if we're in a VS environment
where cl >nul 2>&1
if errorlevel 1 (
    echo ERROR: cl.exe not found. Please run this from a Visual Studio Developer Command Prompt.
    echo.
    echo To open Developer Command Prompt:
    echo   - Search for "Developer Command Prompt" in Start menu
    echo   - Or run: "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
    pause
    exit /b 1
)

REM Build 32-bit DLL (DMC4 is 32-bit)
echo.
echo Compiling for x86 (32-bit)...
cl /LD /EHsc /O2 /MD d3d9_proxy.cpp remix_interface.cpp remix_lighting_manager.cpp lights_tab_ui.cpp imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp imgui/imgui_widgets.cpp imgui/backends/imgui_impl_dx9.cpp imgui/backends/imgui_impl_win32.cpp /link /DEF:d3d9.def /OUT:d3d9.dll

if errorlevel 1 (
    echo.
    echo BUILD FAILED!
    pause
    exit /b 1
)

echo.
echo BUILD SUCCESSFUL!
echo.
echo Output: d3d9.dll
echo.
echo === SETUP INSTRUCTIONS ===
echo 1. In the DMC4 game folder, rename Remix's d3d9.dll to d3d9_remix.dll
echo 2. Copy d3d9.dll (this proxy) to the game folder
echo 3. Copy camera_proxy.ini to the game folder
echo 4. Run the game
echo 5. Check camera_proxy.log for diagnostic output
echo.
echo === DIAGNOSTIC MODE ===
echo To find the correct shader constant registers:
echo 1. Edit camera_proxy.ini
echo 2. Set AutoDetectMatrices=1 (auto-detect and use first matching matrices)
echo    OR Set LogAllConstants=1 (log all constant data for manual analysis)
echo 3. Run the game and check camera_proxy.log
echo.
pause
