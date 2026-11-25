#!/bin/sh

EMU_EXE=parallel_n64

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
HOME="$USERDATA_PATH"
cd "$HOME"

# hardware rendering core: --gl gives it a GLES2 context and a 32-bit window
minarch.elf --gl565 "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM"
