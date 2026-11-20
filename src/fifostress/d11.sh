#!/bin/sh
# d11.sh : (device, tests only) D11: does the display engine run with a different set-up under a GL client than
#   under panflip? Dumps the DE registers the driver programs (offsets from de_atm7059.h only, read only through
#   /dev/mem; no scanning) while (1) fifostress (GL, GPU driver flips) runs, and (2) panflip tri --setvar runs.
#   No wcstress: this compares configuration, not underflows. Output: two dumps to diff (paste both back).
#   MinUI parked, restored after. Writes nothing to the SD card.
D=$(dirname "$0"); B=0xB02E0000
r() { printf "%-14s %s\n" "$1" "$(busybox devmem $(( B + $2 )) 32)"; }
dump() {
  r IF_CON 0x00c; r MMU_EN 0x010; r OUTPUT_CON 0x1000; r WB_CON 0x1004
  for p in 0 1; do o=$(( 0x100 + p * 0x100 ))
    r P${p}_CTL $o; r P${p}_BK $(( o + 0x20 )); r P${p}_SIZE $(( o + 0x24 )); r P${p}_E_COOR $(( o + 0x28 ))
  done
  r PATH_DITHER 0x150
  for n in 0 1 2 3; do o=$(( 0x400 + n * 0x100 ))
    r O${n}_CFG $o; r O${n}_ISIZE $(( o + 0x04 )); r O${n}_OSIZE $(( o + 0x08 )); r O${n}_SR $(( o + 0x0c ))
    r O${n}_BA0 $(( o + 0x30 )); r O${n}_STR $(( o + 0x48 )); r O${n}_CRIT $(( o + 0x4c ))
    r O${n}_REMAP $(( o + 0x50 )); r O${n}_COOR $(( o + 0x54 )); r O${n}_ALPHA $(( o + 0x58 ))
    r O${n}_BLEND $(( o + 0x64 ))
  done
}
echo -17 > /proc/$$/oom_adj
/etc/init.d/S31minui stop >/dev/null 2>&1; sleep 1
cd $D
echo "=== 1: fifostress (GL)"
./fifostress -t 8 -r 8 --fill 1 >/dev/null 2>&1 & F=$!
sleep 4; dump; wait $F
echo "=== 2: panflip tri --setvar"
./panflip 8 tri --at 4000 --setvar >/dev/null & P=$!
sleep 4; dump; wait $P
fbset -g 640 480 640 960 32
setsid /usr/bin/minui </dev/null >/dev/null 2>&1 &
sleep 3
