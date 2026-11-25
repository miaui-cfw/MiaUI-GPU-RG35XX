#!/bin/sh

EMU_EXE=ppsspp

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
# PPSSPP wants its assets (fonts, flash0) in <system dir>/PPSSPP: use the
# standalone install's read-only copy rather than duplicating ~20MB of it
[ -e "$BIOS_PATH/$EMU_TAG/PPSSPP" ] || ln -s /usr/share/ppsspp/PPSSPP "$BIOS_PATH/$EMU_TAG/PPSSPP"
HOME="$USERDATA_PATH"
cd "$HOME"

# hardware rendering core: --gl gives it a GLES2 context and a 32-bit window
minarch.elf --gl "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM"
