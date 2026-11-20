#!/bin/sh
# burst-sweep.sh [SECS] : (device, tests only) measures how long a write-combined CPU burst may last before the DE
#   FIFO underflows. Runs the 20 s reproducer (wc-flip.sh) once per burst size / gap pair and prints the underflows
#   and the achieved rate of each. Writes nothing to the SD card.
#
#   The question it answers: is the trigger the AVERAGE WC rate (~1 GB/s, section 4.2) or the LENGTH of one
#   uninterrupted burst? If small bursts with short gaps stay at 0 while their average rate is still high, the FIFO
#   only covers a burst of a given length, and applications (and an LD_PRELOAD shim) can split their uploads into
#   chunks below that size. The largest clean KB value at a given gap is the per-burst budget.
#
#   Each line of SETTINGS is "KB GAP_US". usleep() on this kernel rounds up to tens of us; the printed MB/s is the
#   real rate. Override with SETTINGS="..." sh burst-sweep.sh.
SECS=${1:-20}
SETTINGS=${SETTINGS:-"1024 0
256 100
128 100
64 100
32 100
16 100
8 100
4 100
64 30
16 30
4 30"}
echo "$SETTINGS" | while read kb gap; do
  printf "burst %5s KB gap %4s us: " $kb $gap
  sh ./wc-flip.sh $SECS --mode set --kb $kb --gap-us $gap | grep -E "wcstress:|underflows" | tr '\n' ' '
  echo
done
