#!/bin/sh
# 构建「运维箱助手.exe」：单文件，内嵌 frpc + 专用密钥；
# 配置/令牌走设备 Web（WinHTTP），远程连接走 stcp + 系统 OpenSSH。
#
# 依赖：mingw64 gcc / windres（本机 C:\mingw64\bin）；
#       载荷 frpc.exe 与 id_ed25519 从 _deploy/exe/payload 拷来（不入 git）。
# 用法：./build.sh            → 运维箱助手.exe
#       OUT=build/x.exe ./build.sh   → 指定输出（正在运行的旧件改名再换，见交付说明）
set -e
cd "$(dirname "$0")"

GCC=/c/mingw64/bin/gcc.exe
WINDRES=/c/mingw64/bin/windres.exe
OUT="${OUT:-运维箱助手.exe}"
PAYLOAD=../../../_deploy/exe/payload

mkdir -p build payload
[ -f "$PAYLOAD/frpc.exe" ]   || { echo "缺 $PAYLOAD/frpc.exe"; exit 1; }
[ -f "$PAYLOAD/id_ed25519" ] || { echo "缺 $PAYLOAD/id_ed25519"; exit 1; }
cp -f "$PAYLOAD/frpc.exe" "$PAYLOAD/id_ed25519" payload/

# RCDATA 的相对路径是按 .rc 所在目录解析的 ⇒ payload 必须在 ../payload
"$WINDRES" -I /c/mingw64/x86_64-w64-mingw32/include src/res.rc -O coff -o build/res.o

"$GCC" -std=c11 -O2 -Wall -Wextra \
    -finput-charset=UTF-8 -fexec-charset=UTF-8 \
    -Isrc \
    -o "$OUT" \
    src/main.c build/res.o \
    -mwindows -static -static-libgcc \
    -lcomctl32 -lcomdlg32 -lshlwapi -lws2_32 -lgdi32 -luser32 -ladvapi32 \
    -lshell32 -lole32 -lwinhttp -liphlpapi -lbcrypt

ls -l "$OUT"
