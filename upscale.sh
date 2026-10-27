#!/bin/sh
# Upscale every dumped texture not yet in the pack (Real-ESRGAN x4plus on the CPU); restart the game to load them
cd "$(dirname "$0")" && exec ~/.local/share/mss-recomp/upscaler-venv/bin/python tools/upscale_textures.py "$@"
