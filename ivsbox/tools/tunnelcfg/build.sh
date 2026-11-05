#!/bin/sh
# 构建「运维箱通道配置.exe」：单文件，不依赖 PowerShell / python / .NET。
# 依赖：mingw64 gcc + windres（本机 C:\mingw64\bin；没有就设 GCC/WINDRES 环境变量）
set -e
cd "$(dirname "$0")"

GCC=${GCC:-/c/mingw64/bin/gcc.exe}
WINDRES=${WINDRES:-/c/mingw64/bin/windres.exe}
OUT="运维箱通道配置.exe"

[ -x "$GCC" ]     || { echo "缺 gcc：$GCC"; exit 1; }
[ -x "$WINDRES" ] || { echo "缺 windres：$WINDRES"; exit 1; }

mkdir -p build
"$WINDRES" -I /c/mingw64/x86_64-w64-mingw32/include src/res.rc -O coff -o build/res.o

"$GCC" -std=c11 -O2 -Wall -Wextra \
    -finput-charset=UTF-8 -fexec-charset=UTF-8 \
    -Isrc \
    -o "$OUT" \
    src/main.c build/res.o \
    -mwindows -static -static-libgcc \
    -lcomdlg32 -lwinhttp -lbcrypt -lws2_32 -lgdi32 -luser32 \
    -ladvapi32 -lshell32 -lole32

ls -l "$OUT"
