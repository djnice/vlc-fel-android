#!/bin/bash
# cadence.sh <seconds> [position ms]: a P7 FEL file in VLC FEL (from 50:00 by default), display cadence from the dvfel meter + GPU overruns
# DEV: adb device (default: ~/.vlcfel-dev), MEDIA: a P7 FEL file URL (default: ~/.vlcfel-media)
DEV=${DEV:-$(cat ~/.vlcfel-dev 2>/dev/null)}
[ -n "$DEV" ] || { echo "set DEV (adb device, e.g. <player-ip>:5555)"; exit 1; }
A="$HOME/platform-tools/adb -s $DEV"
MEDIA=${MEDIA:-$(cat ~/.vlcfel-media 2>/dev/null)}
[ -n "$MEDIA" ] || { echo "set MEDIA"; exit 1; }
POS=${2:-3000000}
$A shell am force-stop org.videolan.vlc.fel
$A shell input keyevent KEYCODE_WAKEUP
$A shell "setprop debug.dvfp.profile 2; logcat -c"
$A shell am start -n org.videolan.vlc.fel/org.videolan.vlc.gui.video.VideoPlayerActivity -a android.intent.action.VIEW \
  -d "$MEDIA" \
  -t video/x-matroska --el position $POS >/dev/null
sleep 8
$A shell "echo 1 > /sys/kernel/debug/dvfel/stats_reset; cat /sys/class/video_composer/drop_cnt | cut -c1-95"
sleep $1
$A shell "grep -E 'input:|display:|gpu jobs|slot waits|GPU job|vsync:|av:' /sys/kernel/debug/dvfel/stats; cat /sys/class/video_composer/drop_cnt | cut -c1-95"
$A logcat -d -s dvfelplay | grep profile | tail -n +3 | awk '{for(i=1;i<=NF;i++) if($i=="over") s+=$(i-1); n++} END {print "GPU: " s " frames over 41.7 ms in " n " x 5 s"}'
$A shell am force-stop org.videolan.vlc.fel
$A shell input keyevent KEYCODE_WAKEUP
