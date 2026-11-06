#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Dune HD firmware plugin "mvc3d": the 3D MVC kernel module at boot (S928X or S905X4), and libmvc3dplay in config/.
# Players that bundle the same module or an older one (VLC FEL, CineFuze) then need no root for it.
# Built on Dune HD's s928x_dvfel plugin.

plugin_install_dir=`dirname "$0"`/..
plugin_install_dir=`cd "$plugin_install_dir"; pwd`
plugin_name=`basename "$plugin_install_dir"`
plugin_tmp_dir="$FS_PREFIX/tmp/plugins/$plugin_name"

mkdir -p "$plugin_tmp_dir"
log_file="$plugin_tmp_dir/ctl.log"
echo "plugin_install_dir = '$plugin_install_dir', Logging into '$log_file'"
exec >>"$log_file" 2>&1
echo "===== START"

legacy_boot_script="$FS_PREFIX/config/boot/mvc3d.sh"
legacy_ko="$FS_PREFIX/config/mvc3d/mvc3d.ko"
my_ko="$plugin_install_dir/mvc3d.ko"
# the Amlogic S905X4 (SC2) build of the module
if [ "$(getprop ro.board.platform)" = sc2 ]; then
    my_ko="$plugin_install_dir/mvc3d_sc2.ko"
fi
my_lib="$plugin_install_dir/libmvc3dplay.so"
my_helper="$plugin_install_dir/libmvc3dhelper.so"
installed_lib="$FS_PREFIX/config/libmvc3dplay.so"
installed_helper="$FS_PREFIX/config/libmvc3dhelper.so"

Log() {
    echo $(date +'%F %T'): "$*"
}

Do() {
    Log "$*"
    "$@"
}

DoRoot() {
    Log "$*"
    /system/dunehd/dunehd_service "do" "$@"
}

Version() {
    strings "$1" | grep '^version=' | head -1 | sed 's/^version=//'
}

Cksum() {
    cksum "$1" | awk '{print $1}'
}

# the later of two module versions (dates, the same form on one SoC)
Later() {
    printf '%s\n%s\n' "$1" "$2" | sort | tail -n 1
}

CopyIfChanged() {
    if [ ! -e "$2" ] || [ "$(Cksum "$1")" != "$(Cksum "$2")" ]; then
        Do rm -f "$2.tmp"
        Do cp "$1" "$2.tmp"
        Do chmod 755 "$2.tmp"
        Do mv -f "$2.tmp" "$2"
    else
        Log "Keeping $2"
    fi
}

Install()
{
    my_version=$(Version "$my_ko")
    Log "$my_ko version: '$my_version'"

    # a player installed a newer module than this plugin's: it stays
    if [ -e "$legacy_ko" ]; then
        legacy_version=$(Version "$legacy_ko")
        Log "$legacy_ko version: '$legacy_version'"
        if [ -n "$legacy_version" ] && [ "$legacy_version" != "$my_version" ] && \
           [ "$(Later "$legacy_version" "$my_version")" = "$legacy_version" ]; then
            Log "A newer module of a player: keeping it"
            return
        fi
    fi
    if [ -e "$legacy_boot_script" ]; then
        Log "Removing the player's boot script: '$legacy_boot_script'"
        DoRoot rm -f "$legacy_boot_script"
    fi

    actual_version=
    if [ -e /sys/module/mvc3d/version ]; then
        actual_version=$(cat /sys/module/mvc3d/version)
        Log "Kernel module is already loaded, version: '$actual_version'"
        if [ "$actual_version" != "$my_version" ] && [ "$(Later "$actual_version" "$my_version")" = "$my_version" ]; then
            Log "Older version, unloading"
            DoRoot rmmod mvc3d
            actual_version=
        fi
    else
        Log "Kernel module is not loaded"
    fi
    if [ "$actual_version" = "" ]; then
        DoRoot insmod "$my_ko"
    fi
    i=0; while [ ! -e /dev/mvc3dhw ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    [ -e /dev/mvc3d ] && DoRoot chmod 666 /dev/mvc3d
    DoRoot chmod 666 /dev/mvc3dhw

    CopyIfChanged "$my_lib" "$installed_lib"
    CopyIfChanged "$my_helper" "$installed_helper"
    Do sync
}

Uninstall()
{
    Do rm -f "$installed_lib" "$installed_helper"
    DoRoot rmmod mvc3d
    sync
}

Log "$0 $*"

action="$1"
case "$action" in
    boot|install)
        Install
        ;;
    uninstall)
        Uninstall
        ;;
    *)
        echo "Usage: $0 (boot|install|uninstall)"
        exit 1
        ;;
esac
