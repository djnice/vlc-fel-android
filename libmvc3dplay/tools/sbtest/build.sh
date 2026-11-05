#!/bin/bash
# WSL: builds sbtest.apk (armeabi-v7a NativeActivity)
set -e
SRC=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HOME/mvc3dplay-work/sbtest}
ANDROID_SDK=${ANDROID_SDK:-$HOME/android-tools/android-sdk-linux}
NDK=${NDK:-$(ls -d "$ANDROID_SDK"/ndk/* | sort -V | tail -1)}
BT=$(ls -d "$ANDROID_SDK"/build-tools/* | sort -V | tail -1)
PLATFORM=$(ls -d "$ANDROID_SDK"/platforms/* | sort -V | tail -1)
KEYSTORE=${KEYSTORE:-$HOME/.android/debug.keystore}
CLANG=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/armv7a-linux-androideabi26-clang
rm -rf "$OUT" && mkdir -p "$OUT/apk/lib/armeabi-v7a"
$CLANG -O2 -Wall -fPIC -shared -o "$OUT/apk/lib/armeabi-v7a/libsbtest.so" "$SRC/sbtest.c" -landroid -llog \
  -u ANativeActivity_onCreate -Wl,-z,max-page-size=16384
"$BT/aapt2" link --manifest "$SRC/AndroidManifest.xml" -I "$PLATFORM/android.jar" -o "$OUT/apk/base.apk"
(cd "$OUT/apk" && zip -q -r base.apk lib)
"$BT/zipalign" -f -p 4 "$OUT/apk/base.apk" "$OUT/apk/aligned.apk"
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android --out "$OUT/sbtest.apk" "$OUT/apk/aligned.apk"
ls -la "$OUT/sbtest.apk"
