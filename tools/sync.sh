#!/bin/bash
# Maintainer tool (WSL): copies the working sources into this repository.
#   kernel/        <- dvfel/kernel (the Android build of the dvfel module)
#   libdvfelplay/  <- dvfelplay (git), without the research scripts
#   vlc/           <- vlc/out-patches, vlc/build-in-docker.sh
#   tools/tests/   <- the test scripts (kbuild.sh, modver.py, fwcheck.sh live here)
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
W=${W:-/mnt/c/dovi_s5}

cp $W/dvfel/kernel/{dvfel.c,dvfel_uapi.h,dvfel_compat.h,Makefile} $R/kernel/

L=$R/libdvfelplay
rm -rf $L && mkdir -p $L/tools
(cd $W/dvfelplay && git ls-files | grep -vE '^tools/' | tar -cf - -T -) | tar -xf - -C $L
cp $W/dvfelplay/tools/{felbench.cpp,bench.sh,bench-run.sh,mkau.py} $L/tools/

rm -f $R/vlc/patches/*.patch
cp $W/vlc/out-patches/0*.patch $R/vlc/patches/
cp $W/vlc/build-in-docker.sh $R/vlc/

cp $W/vlc/tests/{cadence.sh,pauseseek.sh} $R/tools/tests/
echo "synced from $W ($(cd $W/dvfelplay && git log --oneline -1))"
