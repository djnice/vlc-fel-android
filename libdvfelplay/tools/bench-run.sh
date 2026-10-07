#!/bin/bash
# WSL: build felbench, push, run tools/bench.sh on the box (args go to felbench)
set -e
cd "$(dirname "$0")/.." && bash build.sh >/tmp/dvfp-build.log 2>&1 || { grep -E "error" /tmp/dvfp-build.log | head; exit 1; }
grep -E "warning" /tmp/dvfp-build.log | head -5 || true
DEV=${DEV:-$(cat ~/.vlcfel-dev 2>/dev/null)}
[ -n "$DEV" ] || { echo "set DEV (adb device, e.g. <player-ip>:5555)"; exit 1; }
A="$HOME/platform-tools/adb -s $DEV"
$A push ~/dvfelplay-work/build/cmake-armeabi-v7a/felbench /data/local/tmp/ >/dev/null
$A push tools/bench.sh /data/local/tmp/ >/dev/null
$A shell chmod 755 /data/local/tmp/felbench /data/local/tmp/bench.sh
$A shell /data/local/tmp/bench.sh "$@"
