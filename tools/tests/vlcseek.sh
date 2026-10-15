#!/bin/bash
# vlcseek.sh [media] [rounds]: VLC FEL plays a FEL clip, seeks with DPAD_RIGHT / LEFT and measures the FEL
# composition 6-14 s after each seek (composed vs fallback, display gaps, late).
DEV=${DEV:-$(cat ~/.vlcfel-dev 2>/dev/null)}
A="$HOME/platform-tools/adb -s ${DEV:?DEV= (or ~/.vlcfel-dev): the player's adb address}"
MEDIA=${1:-file:///sdcard/Android/data/org.videolan.vlc.fel/files/P7_FEL_STDL2.mkv}
stats() { $A shell "su 0 grep -E 'gpu jobs|display:|vsync:' /sys/kernel/debug/dvfel/stats" | tr -d '\r' | tr '\n' ' ' | sed 's/frames per vsync gap //; s/  */ /g' | cut -c1-170; echo; }
$A shell am force-stop org.videolan.vlc.fel
$A logcat -c
$A shell am start -n org.videolan.vlc.fel/org.videolan.vlc.gui.video.VideoPlayerActivity -a android.intent.action.VIEW -d "$MEDIA" >/dev/null
sleep 10
$A shell "su 0 sh -c 'echo 1 > /sys/kernel/debug/dvfel/stats_reset'"; sleep 8; echo "before: $(stats)"
for r in $(seq 1 ${2:-3}); do
  if [ $((r % 3)) = 0 ]; then K=KEYCODE_DPAD_LEFT; else K=KEYCODE_DPAD_RIGHT; fi
  $A shell input keyevent $K
  sleep 6; $A shell "su 0 sh -c 'echo 1 > /sys/kernel/debug/dvfel/stats_reset'"; sleep 8
  echo "seek $r ($K): $(stats)"
done
$A logcat -d | tr -d '\r' | grep -E "dvfelplay.*(opened|stalled|resent)" | cut -c1-180 | head -5
$A shell am force-stop org.videolan.vlc.fel
