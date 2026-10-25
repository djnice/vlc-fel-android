#!/bin/bash
# Build an external module for the Dune S905X4 (SC2) 5.4.125 kernel.
# usage: kbuild.sh <module dir (WSL path)> [make targets]
# Kernel headers: CoreELEC linux-amlogic 7c5e663^1 (2022-05, the last 5.4.125 Amlogic state),
# worktree ~/sc2fel/k125, prepared in ~/sc2fel/out125 (config of the Dune 5.4.210 build).
# Amlogic layouts are verified against the Dune SC2 binaries field by field (kernel/sc2/dvfel_sc2.c).
# CRCs: modpost takes them from ~/sc2fel/Module.symvers.sc2, generated from the
# __versions/__crc_* of the Dune SC2 modules (vendor_boot + /vendor/lib/modules).
set -e
M=$(readlink -f "$1"); shift || true
TOOLS=$(dirname $(readlink -f $0))
KSRC=~/sc2fel/k125
KOUT=~/sc2fel/out125
FW=~/dunefw/fw/260303_1721_r24_Pro_Vision_4K
SYMVERS=~/sc2fel/Module.symvers.sc2
export LD_LIBRARY_PATH=~/toolchains/compat
export PATH=~/toolchains/clang11/bin:$(ls -d ~/ce22/CoreELEC/build.*/toolchain/bin):$PATH
if [ ! -f $SYMVERS ]; then
	python3 $TOOLS/modver.py db ~/sc2fel/sc2_crc.json $FW/vb/rd/lib/modules/*.ko $FW/modules/*.ko
	python3 $TOOLS/modver.py symvers ~/sc2fel/sc2_crc.json $SYMVERS $FW/vb/rd/lib/modules/*.ko $FW/modules/*.ko
fi
KCFG=$KSRC/arch/arm64/configs/meson64_gki_module_config
PRE="$(for y in $(grep "^CONFIG_.*=y" $KCFG | sed 's/=y//'); do echo -n " -D$y"; done; for m in $(grep "^CONFIG_.*=m" $KCFG | sed 's/=m//'); do echo -n " -D${m}_MODULE"; done)"
make -C $KSRC O=$KOUT ARCH=arm64 LLVM=1 CC=clang LD=ld.lld NM=llvm-nm OBJCOPY=llvm-objcopy AR=llvm-ar \
	OBJDUMP=llvm-objdump STRIP=llvm-strip CLANG_TRIPLE=aarch64-linux-gnu- CROSS_COMPILE=aarch64-libreelec-linux-gnu- \
	M=$M KCFLAGS="$PRE" KBUILD_EXTRA_SYMBOLS=$SYMVERS "${@:-modules}"
for ko in $M/*.ko; do
	[ -f "$ko" ] || continue
	python3 $TOOLS/modver.py fix ~/sc2fel/sc2_crc.json $ko || true
	echo "$(basename $ko): $(modinfo -F vermagic $ko)"
done
