#!/bin/sh
# Launch the recompiled Mario Super Sluggers (extra RT_* env vars pass through)
# PulseAudio backend: SDL3's PipeWire open stalls on this system
export SDL_AUDIO_DRIVER="${SDL_AUDIO_DRIVER:-pulseaudio}"
# collect textures as they appear (each saved once) for ./upscale.sh; RT_TEXDUMP=0 to stop
export RT_TEXDUMP="${RT_TEXDUMP:-1}"
cd "$(dirname "$0")" && exec ./build/mss ../mss-repo/orig/RMBE01/sys/main.dol ../mss-game "$@"
