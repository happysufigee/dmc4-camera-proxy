@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 > build_log.txt 2>&1
cd /d E:\Benchmarks\dmc4_camera_proxy
echo Current directory: %CD% >> build_log.txt
echo. >> build_log.txt
echo Compiling... >> build_log.txt
cl /LD /EHsc /O2 /MD d3d9_proxy.cpp remix_interface.cpp remix_lighting_manager.cpp lights_tab_ui.cpp imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp imgui/imgui_widgets.cpp imgui/backends/imgui_impl_dx9.cpp imgui/backends/imgui_impl_win32.cpp /link /DEF:d3d9.def /OUT:d3d9.dll >> build_log.txt 2>&1
echo. >> build_log.txt
echo Build exit code: %ERRORLEVEL% >> build_log.txt
dir *.dll >> build_log.txt 2>&1
