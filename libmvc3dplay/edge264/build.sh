#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds edge264-mvc (BSD, https://github.com/jens-duttke/edge264-mvc) at 5757e71 with the picture tag patch
# (0001-*.patch: edge264_set_tag(), Edge264Frame.tag) as a static aarch64 library for mvc3dhelper.
#
#   edge264/build.sh [out_dir]      default out_dir: ~/mvc3dplay-work/edge264
#
# Environment: AARCH64_CC / AARCH64_AR (a static capable aarch64 Linux gcc and its ar).
set -e
SRC=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HOME/mvc3dplay-work/edge264}
TC=$(ls -d $HOME/ce22/CoreELEC/build.*/toolchain/bin 2>/dev/null | head -1)
AARCH64_CC=${AARCH64_CC:-$TC/aarch64-libreelec-linux-gnu-gcc}
AARCH64_AR=${AARCH64_AR:-${AARCH64_CC%gcc}ar}
if [ ! -d "$OUT/.git" ]; then
  git clone -q https://github.com/jens-duttke/edge264-mvc.git "$OUT"
fi
cd "$OUT"
git checkout -q -f 5757e71f3decbeced1150e742e802d263fbd0df4
git clean -qfdx
git apply "$SRC"/0001-*.patch
make -s OS=linux STATIC=yes VARIANTS= BUILDTEST=no _BASE_ARCH= CC="$AARCH64_CC" AR="$AARCH64_AR" \
  CFLAGS="-march=armv8.2-a -mtune=cortex-a76 -O3"
ls -la "$OUT/libedge264.a" "$OUT/edge264.h"
