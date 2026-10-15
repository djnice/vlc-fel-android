#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# au_check.sh <dvfptest.apk> <file.au> [seek s]: plays an .au file (tools/mkau.py) with dvfp_test to its end
# and checks the summary: no frame without its RPU (shown as the base layer as it is: a flash), no stalled EL
# decoder, no late frame. E.g. Dune HD's MP4_P7_FEL_STDL.mp4 (about half the access units without EL and RPU):
# libdvfelplay 2026.10.15.1 369 frames without a matching RPU CRC and an EL decoder stall every ~2 s; 2026.10.15.2 0.
# DEV: adb device (default: ~/.vlcfel-dev). Plays nothing while VLC FEL runs (a shared player).
DEV=${DEV:-$(cat ~/.vlcfel-dev 2>/dev/null)}
[ -n "$DEV" ] && [ -f "$1" ] && [ -f "$2" ] || { echo "usage: DEV=<ip:port> $0 dvfptest.apk file.au [seek s]"; exit 1; }
A="${ADB:-$HOME/platform-tools/adb} -s $DEV"
P=org.dvfel.dvfptest
$A shell pidof org.videolan.vlc.fel >/dev/null && { echo "VLC FEL is playing on $DEV"; exit 1; }
$A install -r "$1" >/dev/null || exit 1
$A shell mkdir -p /sdcard/Android/data/$P/files
$A push "$2" /sdcard/Android/data/$P/files/test.au >/dev/null || exit 1
$A shell "setprop debug.dvfp.test.fel 1; setprop debug.dvfp.test.loops 1; setprop debug.dvfp.test.seek ${3:-0}"
$A shell input keyevent KEYCODE_WAKEUP
$A shell logcat -c
L=$(mktemp)
$A logcat -s dvfp_test dvfelplay > "$L" &
LP=$!
$A shell am start -n $P/android.app.NativeActivity >/dev/null
# to the end: dvfp_close logs "stopped" (no force-stop while the decoders run)
for i in $(seq 1 120); do sleep 2; grep -q "stopped," "$L" && break; done
sleep 2
kill $LP
$A shell setprop debug.dvfp.test.seek 0
$A shell am force-stop $P
grep -E "dvfp_test: done|stopped,|stalled" "$L" | cut -c33-
fail=0
grep -q "stopped," "$L" || { echo "FAIL: did not play to the end"; fail=1; }
grep -q "stalled" "$L" && { echo "FAIL: EL decoder stalled"; fail=1; }
nocrc=$(grep -o "[0-9]* frames without a matching RPU CRC" "$L" | tail -1 | cut -d' ' -f1)
# a seek may leave a few frames of the old position without their packet
[ "${nocrc:-0}" -le $([ "${3:-0}" -gt 0 ] && echo 10 || echo 0) ] || { echo "FAIL: $nocrc frames without their RPU"; fail=1; }
grep -q " 0 late" "$L" || { echo "FAIL: late frames"; fail=1; }
rm -f "$L"
[ $fail = 0 ] && echo PASS
exit $fail
