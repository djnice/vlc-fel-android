#!/system/bin/sh
# bench.sh [felbench args]: felbench on the dumped frames with the GPU at its maximum clock
D=/sdcard/Android/data/org.videolan.vlc.debug/files/dvfp_dump
F=/sys/class/devfreq/fe400000.valhall
old=$(cat $F/min_freq)
cat $F/max_freq > $F/min_freq
cd $D && /data/local/tmp/felbench "$@" $(ls *_meta.bin | sed s/_meta.bin// | sort -t f -k2 -n | tr '\n' ' ')
r=$?
echo $old > $F/min_freq
exit $r
