#!/bin/sh

EMU_EXE=km_morpheuscast_xtreme

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
HOME="$USERDATA_PATH"
cd "$HOME"

# hardware rendering core: --gl565 gives it a GLES2 context and a 16-bit renderer
minarch.elf --gl565 "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM"
