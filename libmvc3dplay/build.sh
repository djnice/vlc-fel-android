#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds libmvc3dplay.so (armeabi-v7a, the player side) and mvc3dhelper (a
# static aarch64 program, shipped as libmvc3dhelper.so next to the library).
# Run in WSL/Linux.
#
#   ./build.sh [out_dir]      default out_dir: ~/mvc3dplay-work/build
#
# Environment: ANDROID_SDK (default ~/android-tools/android-sdk-linux), NDK
# (default the newest under $ANDROID_SDK/ndk), AARCH64_CC (a static capable
# aarch64 Linux gcc), EDGE264 (edge264-mvc with picture tags, built for
# aarch64: libedge264.a + edge264.h, see edge264/build.sh), MVC3D_UAPI (dir of mvc3d_uapi.h,
# default ../kernel/mvc3d), MVC3D_KO (the
# mvc3d.ko to embed for mvc3d_module_install, default $MVC3D_UAPI/mvc3d.ko).
set -e
SRC=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HOME/mvc3dplay-work/build}
ANDROID_SDK=${ANDROID_SDK:-$HOME/android-tools/android-sdk-linux}
NDK=${NDK:-$(ls -d "$ANDROID_SDK"/ndk/* | sort -V | tail -1)}
AARCH64_CC=${AARCH64_CC:-$(ls -d $HOME/ce22/CoreELEC/build.*/toolchain/bin | head -1)/aarch64-libreelec-linux-gnu-gcc}
EDGE264=${EDGE264:-$HOME/mvc3dplay-work/edge264}
MVC3D_UAPI=${MVC3D_UAPI:-$SRC/../kernel/mvc3d}
MVC3D_KO=${MVC3D_KO:-$MVC3D_UAPI/mvc3d.ko}
[ -f "$MVC3D_KO" ] || { echo "no kernel module $MVC3D_KO (MVC3D_KO=)"; exit 1; }
MVC3D_KO=$(readlink -f "$MVC3D_KO")
CLANG=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/armv7a-linux-androideabi26-clang

mkdir -p "$OUT/armeabi-v7a"
$CLANG -O2 -Wall -Wextra -Wno-unused-parameter -fPIC -shared -fvisibility=hidden \
  -I"$SRC/include" -I"$SRC/src" -I"$MVC3D_UAPI" -DMVC3D_HAVE_KO -DMVC3D_KO="\"$MVC3D_KO\"" \
  -o "$OUT/armeabi-v7a/libmvc3dplay.so" \
  "$SRC/src/mvc3dplay.c" "$SRC/src/module_install.c" "$SRC/src/module_blob.S" \
  -llog -ldl -Wl,-z,max-page-size=16384 -Wl,--gc-sections
$AARCH64_CC -O3 -march=armv8.2-a -mtune=cortex-a76 -Wall -static -s \
  -I"$EDGE264" -I"$MVC3D_UAPI" -I"$SRC/src" -o "$OUT/armeabi-v7a/libmvc3dhelper.so" \
  "$SRC/helper/mvc3dhelper.c" "$EDGE264/libedge264.a" -lpthread -lm
ls -la "$OUT/armeabi-v7a/"
