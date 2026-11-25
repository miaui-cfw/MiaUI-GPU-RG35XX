#!/bin/sh

EMU_EXE=genesis_plus_gx
EMU_PAP=genesis_plus_gx_paprium

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"

if echo "$ROM" | grep -iq "paprium"; then
    EMU_EXE=$EMU_PAP
fi

mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
HOME="$USERDATA_PATH"
cd "$HOME"
minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM"
