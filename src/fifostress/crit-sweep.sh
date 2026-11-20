#!/bin/sh
# crit-sweep.sh [SECS] : (device, tests only; kernel with patch 0009) runs the 20 s underflow reproducer
#   (wc-flip.sh: fifostress flips + wcstress write-combined stores) once per DE critical setting, through
#   /sys/module/owl_dss/parameters, and prints the underflows of each. Restores the vendor defaults after.
#   Each line of SETTINGS is "rewrite ctl thresh num". Writes nothing to the SD card.
SECS=${1:-20}; P=/sys/module/owl_dss/parameters
set_crit() { echo $1 > $P/crit_rewrite; echo $2 > $P/crit_ctl; echo $3 > $P/crit_thresh; echo $4 > $P/crit_num; }
SETTINGS=${SETTINGS:-"1 0 16 -1
0 0 16 -1
1 0 64 -1
1 0 128 -1
1 0 255 -1
1 1 16 -1
1 3 16 -1
1 0 16 1
1 0 16 8
1 0 16 32"}
echo "$SETTINGS" | while read rw ctl th num; do
  set_crit $rw $ctl $th $num
  printf "rewrite %s ctl %s thresh %s num %s: " $rw $ctl $th $num
  sh /tmp/fs/wc-flip.sh $SECS --mode set | grep RESULT
done
set_crit 1 0 16 -1
