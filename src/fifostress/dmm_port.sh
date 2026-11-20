#!/bin/sh
# dmm_port.sh OFFSET lo|hi VALUE | --restore | --show : (TEST ONLY, device; user-approved DDR controller writes)
#   Sets one 16-bit half of a DDR controller (DMM 0xB0290000) port register and loads it the way the vendor
#   bootloader does: SCH_CTRL bit 2 cleared, then set. --restore puts back the vendor bootloader values
#   (CPU/BISP 0x0818F808, ETHERNET/DE 0x0000F801, DE_CRITICAL/USB2 0xF8010000, GPU3D/DAP 0, AXI_PRIORITY 0). A reboot also restores
#   everything (the bootloader re-initialises the DDR controller).
B=0xb0290000
r() { busybox devmem $(( B + $1 )) 32; }
w() { busybox devmem $(( B + $1 )) 32 $2; }
load() { S=$(r 0); w 0 $(( S & ~4 )); w 0 $(( S | 4 )); }
show() { for o in 0x00 0x04 0x08 0x0c 0x10 0x14 0x18 0x1c 0x20 0x24 0x28 0x2c 0x64 0x68 0x80; do printf "+%s=%s " $o $(r $o); done; echo; }
case "$1" in
  --show|"") show; exit 0 ;;
  --restore) w 0x04 0x0818F808; w 0x0c 0x0000F801; w 0x10 0xF8010000; w 0x18 0; w 0x2c 0; load; show; exit 0 ;;
esac
OFF=$(($1)); OLD=$(r $OFF); VAL=$(($3))
case "$2" in
  lo) NEW=$(( (OLD & 0xFFFF0000) | (VAL & 0xFFFF) )) ;;
  hi) NEW=$(( (OLD & 0x0000FFFF) | ((VAL & 0xFFFF) << 16) )) ;;
  *) echo "half must be lo or hi"; exit 1 ;;
esac
w $OFF $NEW; load; show
