#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds libdvfelplay.so and the dvfp_test APK (armeabi-v7a, the Dune HD /
# R_volution userspace is 32-bit only). Run in WSL/Linux.
#
#   ./build.sh [out_dir]      default out_dir: ~/dvfelplay-work/build
#
# Environment: ANDROID_SDK (default ~/android-tools/android-sdk-linux), NDK
# (default the newest under $ANDROID_SDK/ndk), LIBDOVI_DIR (libdovi built for
# arm-linux-androideabi with cargo-c), ABI (armeabi-v7a), KEYSTORE (debug keystore), DVFEL_KO (the
# kernel module to bundle, default ~/dvfel-android/dvfel-lead/dvfel.ko), DVFEL_KO_SC2 (the SC2
# module to bundle as well, optional).
set -e
SRC=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HOME/dvfelplay-work/build}
ANDROID_SDK=${ANDROID_SDK:-$HOME/android-tools/android-sdk-linux}
NDK=${NDK:-$(ls -d "$ANDROID_SDK"/ndk/* | sort -V | tail -1)}
ABI=${ABI:-armeabi-v7a}
LIBDOVI_DIR=${LIBDOVI_DIR:-$HOME/android-tools/xbmc-depends/arm-linux-androideabi-24-release}
BT=$(ls -d "$ANDROID_SDK"/build-tools/* | sort -V | tail -1)
PLATFORM=$(ls -d "$ANDROID_SDK"/platforms/* | sort -V | tail -1)
KEYSTORE=${KEYSTORE:-$HOME/.android/debug.keystore}
DVFEL_KO=${DVFEL_KO:-$HOME/dvfel-android/dvfel-lead/dvfel.ko}
[ -f "$DVFEL_KO" ] || { echo "no kernel module $DVFEL_KO (DVFEL_KO=)"; exit 1; }
DVFEL_KO=$(readlink -f "$DVFEL_KO")
# the SC2 (S905X4) module to bundle as well (optional)
DVFEL_KO_SC2=${DVFEL_KO_SC2:+$(readlink -f "$DVFEL_KO_SC2")}
# cmake and ninja: the system's, else the maintainer's native tools
command -v cmake >/dev/null && command -v ninja >/dev/null ||
  PATH=$HOME/android-tools/xbmc-depends/x86_64-linux-gnu-native/bin:$PATH

mkdir -p "$OUT"
cmake -S "$SRC" -B "$OUT/cmake-$ABI" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$ABI" -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_static \
  -DLIBDOVI_DIR="$LIBDOVI_DIR" -DDVFEL_KO="$DVFEL_KO" -DDVFEL_KO_SC2="$DVFEL_KO_SC2" >/dev/null
cmake --build "$OUT/cmake-$ABI"

# the test APK: no Java code (NativeActivity)
APK="$OUT/apk"
rm -rf "$APK" && mkdir -p "$APK/lib/$ABI"
cp "$OUT/cmake-$ABI/libdvfelplay.so" "$OUT/cmake-$ABI/libdvfptest.so" "$APK/lib/$ABI/"
"$BT/aapt2" link --manifest "$SRC/test/AndroidManifest.xml" -I "$PLATFORM/android.jar" \
  -o "$APK/base.apk"
(cd "$APK" && zip -q -r base.apk lib)
"$BT/zipalign" -f -p 4 "$APK/base.apk" "$APK/aligned.apk"
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
  --out "$OUT/dvfptest.apk" "$APK/aligned.apk"
"$NDK"/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm -D --defined-only \
  "$OUT/cmake-$ABI/libdvfelplay.so" | awk '{print $3}' | grep -v '^dvfp_' | sed 's/^/unexpected export: /' || true
ls -la "$OUT/cmake-$ABI/libdvfelplay.so" "$OUT/dvfptest.apk"
