#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# WSL: tools/hwplaytest for armeabi-v7a (the box's userspace), with the library's hardware path compiled in.
#   tools/build-hwplaytest.sh [out_dir]    MVC3D_UAPI: dir of mvc3d_uapi.h (default ../kernel/mvc3d)
set -e
SRC=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$HOME/mvc3dplay-work/build}
ANDROID_SDK=${ANDROID_SDK:-$HOME/android-tools/android-sdk-linux}
NDK=${NDK:-$(ls -d "$ANDROID_SDK"/ndk/* | sort -V | tail -1)}
MVC3D_UAPI=${MVC3D_UAPI:-$SRC/../kernel/mvc3d}
CLANG=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/armv7a-linux-androideabi26-clang
mkdir -p "$OUT"
$CLANG -O2 -Wall -Wextra -Wno-unused-parameter -I"$SRC/include" -I"$MVC3D_UAPI" -o "$OUT/hwplaytest" \
  "$SRC/tools/hwplaytest.c" "$SRC/src/mvc3dhw.c" "$SRC/src/mvcfnr.c" -llog
ls -la "$OUT/hwplaytest"
