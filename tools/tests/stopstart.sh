#!/bin/bash
# stopstart.sh [rounds]: quick stop / start / seek cycles in VLC FEL (Dune's report: a vsync interrupt panic in
# videocomposer_vf_put via dvfel_put after a quick stop, restart and an immediate seek), then checks that the player
# did not panic in dvfel and that dvfel neither refused a frame nor lost a slot reservation. A panic elsewhere
# (the firmware's H.265 decoder panics in vh265_back_irq_cb under such cycles, with or without the fix) is
# saved from pstore and counted; the test then goes on after the reboot.
# DEV: adb device (default: ~/.vlcfel-dev), MEDIA: one or more space separated P7 FEL file URLs (default:
# ~/.vlcfel-media), KO: a local dvfel.ko to load first (rmmod + insmod, again after each reboot), SEED: random
# seed, MAXBOOT: panics outside dvfel tolerated (3), OUT: output directory, KEEP: leave VLC FEL running.
DEV=${DEV:-$(cat ~/.vlcfel-dev 2>/dev/null)}
[ -n "$DEV" ] || { echo "set DEV (adb device, e.g. <player-ip>:5555)"; exit 1; }
A="$HOME/platform-tools/adb -s $DEV"
MEDIA=${MEDIA:-$(cat ~/.vlcfel-media 2>/dev/null)}
[ -n "$MEDIA" ] || { echo "set MEDIA"; exit 1; }
read -r -a FILES <<< "$MEDIA"
ROUNDS=${1:-40}
RANDOM=${SEED:-$$}
PKG=org.videolan.vlc.fel
ACT=$PKG/org.videolan.vlc.gui.video.VideoPlayerActivity
OUT=${OUT:-stopstart-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT"

sh_() { $A shell "su 0 sh -c '$1'" | tr -d '\r'; }
boot_id() { sh_ "cat /proc/sys/kernel/random/boot_id"; }
# random sleep between $1 and $2 ms
nap() { sleep "$(awk -v a="$1" -v b="$2" -v r="$RANDOM" 'BEGIN { printf "%.3f", (a + r % (b - a + 1)) / 1000 }')"; }
play() {
	local f=${FILES[$((RANDOM % ${#FILES[@]}))]} t
	case ${f,,} in
	*.mkv) t=video/x-matroska ;; *.mp4) t=video/mp4 ;; *.ts|*.m2ts) t=video/mp2t ;; *) t=video/* ;;
	esac
	$A shell am start -n $ACT -a android.intent.action.VIEW -d "$f" -t "$t" >/dev/null
}
seeks() { for _ in $(seq 1 $((1 + RANDOM % 3))); do
	if [ $((RANDOM % 3)) = 0 ]; then $A shell input keyevent KEYCODE_DPAD_LEFT; else $A shell input keyevent KEYCODE_DPAD_RIGHT; fi
	nap 50 400; done; }

# KO: replace the loaded dvfel. /dev/dvfel must be rw for the app (as the boot script does), else VLC FEL
# installs its bundled module again.
reload() {
	[ -n "$KO" ] || return 0
	$A shell am force-stop $PKG
	$A push "$KO" /data/local/tmp/dvfel-stress.ko >/dev/null || exit 1
	sh_ "rmmod dvfel; insmod /data/local/tmp/dvfel-stress.ko && for i in 1 2 3 4 5 6 7 8 9 10; do [ -e /dev/dvfel ] && break; sleep 0.2; done; chmod 666 /dev/dvfel && echo reloaded" |
		grep -q reloaded || { echo "dvfel reload failed"; exit 1; }
	echo "loaded $KO"
}
start() {
	$A shell am force-stop $PKG
	reload
	$A shell input keyevent KEYCODE_WAKEUP
	sh_ "dmesg -c >/dev/null; echo 1 > /sys/kernel/debug/dvfel/stats_reset"
	BOOT=$(boot_id)
}
# after a reboot: save the panic of the last boot; dvfel in its call trace fails the test, another module's
# (e.g. the H.265 decoder's vh265_back_irq_cb) is counted and the test goes on (at most MAXBOOT times)
reboots=0
after_reboot() {
	local f="$OUT/console-ramoops-$1.txt"
	for _ in $(seq 1 30); do $A shell true 2>/dev/null && break; ~/platform-tools/adb connect "$DEV" >/dev/null 2>&1; sleep 5; done
	sleep 20
	sh_ "cat /sys/fs/pstore/console-ramoops*" > "$f"
	local pc trace
	pc=$(grep -m1 -oE "pc : [^ ]+( \[[^]]+\])?" "$f")
	trace=$(sed -n '/Call trace:/,/end trace/p' "$f" | grep -oE "[a-z_0-9]+\+0x[0-9a-f]+/0x[0-9a-f]+( \[[^]]+\])?" | head -8 | tr '\n' ' ')
	echo "  ${pc:-no oops in pstore}; trace: $trace(pstore: $f)"
	if [ -z "$pc" ] || echo "$pc $trace" | grep -q dvfel; then return 1; fi
	reboots=$((reboots + 1))
	[ $reboots -le "${MAXBOOT:-3}" ] || return 1
	echo "  not in dvfel: counted ($reboots), going on"
	start
}

start
echo "$ROUNDS rounds on $DEV (boot $BOOT), seed ${SEED:-$$}, output $OUT/"

fail=0
for r in $(seq 1 "$ROUNDS"); do
	case $((RANDOM % 3)) in
	0) how="force-stop"; $A shell am force-stop $PKG; nap 100 1200 ;;
	1) how="back"; $A shell input keyevent KEYCODE_BACK; nap 100 1200 ;;
	*) how="new intent" ;;		# VLC stops the current file and opens the next
	esac
	play
	nap 300 2500			# Dune: the seek came right after the (re)start
	seeks
	nap 500 4000
	b=$(boot_id)
	if [ "$b" != "$BOOT" ]; then
		echo "round $r ($how): the player rebooted"
		after_reboot "$r" && continue
		fail=1
		break
	fi
	bad=$(sh_ "dmesg -c" | tee -a "$OUT/dmesg.txt" | grep -E "dvfel: .*(not an upstream frame|lost its reservation|taken again|record full)|Unable to handle|Internal error")
	[ -n "$bad" ] && { echo "round $r ($how):"; echo "$bad" | sed 's/^/  /'; fail=1; }
	printf "round %d/%d (%s): %s\n" "$r" "$ROUNDS" "$how" "$(sh_ "grep -E \"^upstream frames\" /sys/kernel/debug/dvfel/stats")"
done

sh_ "cat /sys/kernel/debug/dvfel/stats" > "$OUT/stats.txt"
[ -n "$KEEP" ] || $A shell am force-stop $PKG
if [ $fail = 0 ]; then
	echo "PASS: no dvfel panic, no refused frame, no lost slot ($ROUNDS rounds, $reboots panics elsewhere)"
else
	echo "FAIL (see $OUT/)"
fi
exit $fail
