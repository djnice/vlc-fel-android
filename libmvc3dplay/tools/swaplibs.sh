#!/bin/bash
# swaplibs.sh: build libmvc3dplay + helper and replace them in the installed VLC FEL on $DEV (root; test only)
DEV=${DEV:-$(cat ~/.vlcfel-dev 2>/dev/null)}
A="$HOME/platform-tools/adb -s $DEV"
cd /mnt/c/dovi_s5/mvc3dplay && MVC3D_UAPI=${MVC3D_UAPI:-/mnt/c/dovi_s5/dvfel/mvc3d} MVC3D_KO=${MVC3D_KO:-$HOME/mvc3d-build/mvc3d.ko} bash build.sh | tail -2
$A shell am force-stop org.videolan.vlc.fel
for f in libmvc3dplay.so libmvc3dhelper.so; do $A push ~/mvc3dplay-work/build/armeabi-v7a/$f /data/local/tmp/$f >/dev/null; done
$A shell "su 0 sh -c 'L=\$(ls -d /data/app/*/org.videolan.vlc.fel*/lib/arm); for f in libmvc3dplay.so libmvc3dhelper.so; do cat /data/local/tmp/\$f > \$L/\$f; done; ls -la \$L | grep mvc3d'"
