#!/bin/sh
# 用 zig 交叉编译各平台二进制
#   ZIG=/path/to/zig ./scripts/build.sh
set -e
cd "$(dirname "$0")/.."

ZIG="${ZIG:-zig}"
OUT="build"
mkdir -p "$OUT"

echo "[1/4] Windows x64"
"$ZIG" c++ -target x86_64-windows-gnu -O2 -s -std=c++17 src/proxy_core.cpp \
    -o "$OUT/proxy_core_win_x64.exe" -lws2_32

echo "[2/4] Linux x86_64"
"$ZIG" c++ -target x86_64-linux-musl -O2 -s -std=c++17 src/proxy_core.cpp \
    -o "$OUT/proxy_core_linux_x64" -static -lpthread

echo "[3/4] Linux ARM64 (aarch64)"
"$ZIG" c++ -target aarch64-linux-musl -O2 -s -std=c++17 src/proxy_core.cpp \
    -o "$OUT/proxy_core_linux_arm64" -static -lpthread

echo "[4/4] Linux ARM32 (armhf)"
"$ZIG" c++ -target arm-linux-musleabihf -O2 -s -std=c++17 src/proxy_core.cpp \
    -o "$OUT/proxy_core_linux_arm32" -static -lpthread

ls -l "$OUT"
