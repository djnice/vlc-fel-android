#!/bin/bash
# VLC FEL: VLC for Android (libvlcjni-3.x, VLC 3.0.x) with the dvfel patches, armeabi-v7a.
# Runs in registry.videolan.org/vlc-debian-android-3.0:20260701183656 with
#   /work: build volume; PATCHES: the dvfel patches (0*.patch), DVFELPLAY_SO: the
#   libdvfelplay.so to bundle (defaults: the maintainer's tree mounted at /host).
# Output: /work/vlc-android/application/app/build/outputs/apk/dev/, signed with
# the container's throwaway debug key: re-sign it (apksigner) with your own.
set -e
PATCHES=${PATCHES:-/host/vlc/out-patches}
DVFELPLAY_SO=${DVFELPLAY_SO:-/host/vlc/dvfel-libs/armeabi-v7a/libdvfelplay.so}
VLC_ANDROID_HASH=fc5fa1f4ee0d42b79dca60747aeb9c73cafe6ad1
LIBVLCJNI_HASH=0b8dc65efb203c86a0476bc337ddd99ecf1c0ef6

cd /work
if [ ! -d vlc-android/.git ]; then
  git clone https://code.videolan.org/videolan/vlc-android.git
  (cd vlc-android && git checkout -q $VLC_ANDROID_HASH)
fi
cd vlc-android
if [ ! -d libvlcjni/.git ]; then
  git clone --single-branch -b libvlcjni-3.x https://code.videolan.org/videolan/libvlcjni.git
  (cd libvlcjni && git reset -q --hard $LIBVLCJNI_HASH)
fi

# the dvfel patches (out-patches/000N-*), applied by get-vlc.sh after the
# libvlcjni patches as 002N-*; an already cloned vlc gets the missing ones
n=20
for p in "$PATCHES"/0*.patch; do
  n=$((n + 1))
  dst=libvlcjni/libvlc/patches/00$n-dvfel-$(basename "$p" | cut -c6-)
  rm -f libvlcjni/libvlc/patches/00$n-*.patch
  cp "$p" "$dst"
  id=$(sed -n 's/^Message-Id: //p' "$p" | head -1)
  if [ -d libvlcjni/vlc/.git ] && ! (cd libvlcjni/vlc && git log --grep="$id" --oneline | grep -q .); then
    (cd libvlcjni/vlc && git am --message-id "../libvlc/patches/$(basename "$dst")")
  fi
done

# libdvfelplay.so next to libvlc.so in the APK (ndk-build cleans jni/libs)
mkdir -p libvlcjni/libvlc/dvfel-libs/armeabi-v7a
cp "$DVFELPLAY_SO" libvlcjni/libvlc/dvfel-libs/armeabi-v7a/
# 3D MVC: libmvc3dplay.so and its helper (a static aarch64 program named
# libmvc3dhelper.so so that it is extracted next to the libraries)
MVC3D_DIR=${MVC3D_DIR:-/host/vlc/dvfel-libs/armeabi-v7a}
for f in libmvc3dplay.so libmvc3dhelper.so; do
  [ -f "$MVC3D_DIR/$f" ] && cp "$MVC3D_DIR/$f" libvlcjni/libvlc/dvfel-libs/armeabi-v7a/
done
ls -la libvlcjni/libvlc/dvfel-libs/armeabi-v7a/
grep -q dvfel-libs libvlcjni/libvlc/build.gradle ||
  sed -i "s|jniLibs.srcDirs = \[ 'jni/libs' \]|jniLibs.srcDirs = [ 'jni/libs', 'dvfel-libs' ]|" libvlcjni/libvlc/build.gradle
grep -n "jniLibs" libvlcjni/libvlc/build.gradle

# no prebuilt contribs for this contrib hash (404): built from source; the
# librist tarball (GitLab generated) no longer matches its checksum, RIST
# streaming is not needed
grep -q -- "--disable-librist" libvlcjni/buildsystem/compile-libvlc.sh ||
  sed -i 's|^    --disable-srt \\$|    --disable-srt \\\n    --disable-librist \\|' libvlcjni/buildsystem/compile-libvlc.sh
grep -n -- "--disable-librist" libvlcjni/buildsystem/compile-libvlc.sh

# VLC FEL: own package (org.videolan.vlc.fel, next to the official VLC) and name
sed -i 's|applicationIdSuffix ".debug"|applicationIdSuffix ".fel"|' application/app/build.gradle
sed -i 's|appId}\.debug|appId}.fel|g' application/resources/build.gradle application/vlc-android/build.gradle
sed -i 's|"org\.videolan\.vlc\.debug"|"org.videolan.vlc.fel"|g' application/vlc-android/flavors/debug/res/xml/shortcuts.xml
grep -n 'applicationIdSuffix' application/app/build.gradle
grep -n 'appId}\.' application/resources/build.gradle application/vlc-android/build.gradle
sed -i 's|<string name="app_name" translatable="false">VLC</string>|<string name="app_name" translatable="false">VLC FEL</string>|' \
  application/resources/src/main/res/values/strings.xml
grep -n '"app_name"' application/resources/src/main/res/values/strings.xml

git config --global user.email djnice@freemail.hu
git config --global user.name djnice
buildsystem/compile.sh -a armeabi-v7a -t "$@"
ls -la application/app/build/outputs/apk/*/ 2>/dev/null || find . -name "*.apk" -newer /work/vlc-android/build.gradle | head
