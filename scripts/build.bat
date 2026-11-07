@echo off
REM Cross-compile all targets with zig
REM   set ZIG=D:\zig\zig.exe  then run scripts\build.bat
setlocal
cd /d "%~dp0.."
if "%ZIG%"=="" set ZIG=zig
if not exist build mkdir build

echo [1/4] Windows x64
"%ZIG%" c++ -target x86_64-windows-gnu -O2 -s -std=c++17 src\proxy_core.cpp -o build\proxy_core_win_x64.exe -lws2_32 || goto :err

echo [2/4] Linux x86_64
"%ZIG%" c++ -target x86_64-linux-musl -O2 -s -std=c++17 src\proxy_core.cpp -o build\proxy_core_linux_x64 -static -lpthread || goto :err

echo [3/4] Linux ARM64 (aarch64)
"%ZIG%" c++ -target aarch64-linux-musl -O2 -s -std=c++17 src\proxy_core.cpp -o build\proxy_core_linux_arm64 -static -lpthread || goto :err

echo [4/4] Linux ARM32 (armhf)
"%ZIG%" c++ -target arm-linux-musleabihf -O2 -s -std=c++17 src\proxy_core.cpp -o build\proxy_core_linux_arm32 -static -lpthread || goto :err

echo.
echo done.
dir /b build
exit /b 0

:err
echo build failed
exit /b 1
