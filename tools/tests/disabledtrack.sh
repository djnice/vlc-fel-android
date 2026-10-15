#!/bin/bash
# disabledtrack.sh [media]: VLC FEL plays a clip whose only video track has FlagEnabled 0 (patch 0017:
# it must still get a picture): after 12 s the HWC layers (video plane DEVICE), the mode, dvfelplay.
# The clip: P7_FEL_STDL1.mkv with its video track's FlagLacing 0 element rewritten as FlagEnabled 0.
DEV=${DEV:-$(cat ~/.vlcfel-dev 2>/dev/null)}
A="$HOME/platform-tools/adb -s ${DEV:?DEV= (or ~/.vlcfel-dev): the player's adb address}"
MEDIA=${1:-file:///sdcard/Android/data/org.videolan.vlc.fel/files/P7_FEL_STDL1_disabled.mkv}
$A shell am force-stop org.videolan.vlc.fel
$A logcat -c
$A shell am start -n org.videolan.vlc.fel/org.videolan.vlc.gui.video.VideoPlayerActivity -a android.intent.action.VIEW \
  -d "$MEDIA" >/dev/null
sleep 12
echo "mode: $($A shell su 0 cat /sys/class/display/mode | tr -d '\r')"
$A shell dumpsys SurfaceFlinger | tr -d '\r' | grep -A10 "HWC layers:" | grep -E "DEVICE|SIDEBAND" | head -3
$A logcat -d | tr -d '\r' | grep -E "dvfelplay.*opened|libvlc decoder: Dolby|disabled, but|vout display" | cut -c1-200 | head -5
$A shell "su 0 grep -E 'gpu jobs|display:' /sys/kernel/debug/dvfel/stats" | tr -d '\r'
$A shell am force-stop org.videolan.vlc.fel
