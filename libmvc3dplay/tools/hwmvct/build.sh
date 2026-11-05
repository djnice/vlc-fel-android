#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# WSL: hwmvct.ko (test receiver for tools/hwmvctest) for the Dune 5.4.210 GKI kernel, and
# tools/hwmvctest (static aarch64).  KBUILD: the repository's tools/kbuild.sh;
# AARCH64_CC: a static capable aarch64 Linux gcc.
set -e
S=$(cd "$(dirname "$0")" && pwd)
SRC=$S/../..
B=${B:-$HOME/hwmvct-build}
KBUILD=${KBUILD:-$SRC/../tools/kbuild.sh}
AARCH64_CC=${AARCH64_CC:-$(ls -d $HOME/ce22/CoreELEC/build.*/toolchain/bin | head -1)/aarch64-libreelec-linux-gnu-gcc}
mkdir -p $B
cp $S/hwmvct.c $S/hwmvct_uapi.h $S/Makefile $B/
bash $KBUILD $B 2>&1 | grep -vE "^\s*(CC|LD|MODPOST|LTO|Building)|Entering|Leaving|Symbol version dump|modules will have no|^$"
$AARCH64_CC -O2 -Wall -static -march=armv8.2-a -I$SRC/include -I$S -o $B/hwmvctest $SRC/tools/hwmvctest.c $SRC/src/mvcfnr.c -lpthread
ls -la $B/hwmvct.ko $B/hwmvctest
