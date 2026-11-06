#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# WSL: mvc3d.ko for the Dune S905X4 (SC2) 5.4.125 kernel (hardware MVC through amvideo) -> this dir
set -e
S=$(dirname "$(readlink -f "$0")")
R=$S/../../..
B=~/sc2mvc/mvc3d; rm -rf "$B"; mkdir -p "$B"
cp "$S/mvc3d_sc2.c" "$S/Makefile" "$S/../mvc3d_uapi.h" "$B/"
bash "$R/tools/kbuild-sc2.sh" "$B"
cp "$B/mvc3d.ko" "$S/mvc3d.ko"
python3 "$R/tools/modver.py" show "$S/mvc3d.ko" | grep -vE "^0x" || true
ls -la "$S/mvc3d.ko"
