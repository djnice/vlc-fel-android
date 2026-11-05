#!/system/bin/sh
# Box (as root): HW MVC decode check. Loads hwmvct.ko, routes the legacy decoder path to it,
# runs hwmvctest with the given arguments, restores the vfm map and the decoder parameters.
#   su 0 sh /data/local/tmp/hwmvc/hwmvctest.sh <out.crc> [hwmvctest args...] in.264
D=/data/local/tmp/hwmvc
MAP=/sys/class/vfm/map
P=/sys/module/amvdec_h264mvc/parameters
OUT=$1; shift
grep -q "^hwmvct " /proc/modules || insmod $D/hwmvct.ko || exit 1
OLD_DROP=$(cat $P/drop_thread_hold 2>/dev/null)
OLD_VIEW=$(cat $P/view_mode)
restore() {
	echo "rm hwmvct" > $MAP
	grep -q "default {" $MAP || echo "add default decoder ppmgr deinterlace amvideo" > $MAP
	[ -n "$OLD_DROP" ] && echo $OLD_DROP > $P/drop_thread_hold
	echo $OLD_VIEW > $P/view_mode
}
trap restore EXIT INT TERM
echo "rm default" > $MAP
echo "add hwmvct decoder hwmvct" > $MAP
[ -n "$OLD_DROP" ] && echo 0 > $P/drop_thread_hold
echo 3 > $P/view_mode
cd $D
./hwmvctest "$@" > $OUT
