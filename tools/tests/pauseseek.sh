#!/bin/bash
# pauseseek.sh: a P7 FEL file in VLC FEL, pause/resume and seeks, then the dvfel stats (lead time robustness)
# DEV: adb device (default: ~/.vlcfel-dev), MEDIA: a P7 FEL file URL (default: ~/.vlcfel-media)
DEV=${DEV:-$(cat ~/.vlcfel-dev 2>/dev/null)}
[ -n "$DEV" ] || { echo "set DEV (adb device, e.g. <player-ip>:5555)"; exit 1; }
A="$HOME/platform-tools/adb -s $DEV"
MEDIA=${MEDIA:-$(cat ~/.vlcfel-media 2>/dev/null)}
[ -n "$MEDIA" ] || { echo "set MEDIA"; exit 1; }
$A shell am force-stop org.videolan.vlc.fel
$A shell input keyevent KEYCODE_WAKEUP
$A shell "logcat -c; dmesg -c >/dev/null"
$A shell am start -n org.videolan.vlc.fel/org.videolan.vlc.gui.video.VideoPlayerActivity -a android.intent.action.VIEW \
  -d "$MEDIA" \
  -t video/x-matroska --el position 3000000 >/dev/null
sleep 10
st() { $A shell "grep -E 'frames in|display:|av:|vsync:' /sys/kernel/debug/dvfel/stats" | cut -c1-120; }
$A shell "echo 1 > /sys/kernel/debug/dvfel/stats_reset"
echo "== pause 4 s"; $A shell input keyevent KEYCODE_MEDIA_PAUSE; sleep 4; st
echo "== play 6 s"; $A shell input keyevent KEYCODE_MEDIA_PLAY; sleep 6; st
for i in 1 2 3; do echo "== seek +$i"; $A shell input keyevent KEYCODE_DPAD_RIGHT; sleep 5; done; st
echo "== seek back"; $A shell input keyevent KEYCODE_DPAD_LEFT; sleep 8; st
$A shell "echo 1 > /sys/kernel/debug/dvfel/stats_reset"; sleep 20; echo "== 20 s steady"; st
$A shell "dmesg | grep -iE 'dvfel|vc:' | grep -viE 'early release|min_freq' | tail -8"
[ -n "$KEEP" ] || $A shell am force-stop org.videolan.vlc.fel
