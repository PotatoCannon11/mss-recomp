# Status (updated by Claude while you're out)

Check in over SSH:
    cat ~/mss-decomp/recomp/STATUS.md          # this file
    cd ~/mss-decomp/recomp && git log --oneline | head
    ls -t build/frames/*.png | head            # latest screenshots (PNG)

## 2026-10-26 morning
- Recompiled game boots natively on Linux: strap screen, save prompt, intro movie (video + audio),
  title screen ("PRESS A+B") and the in-game LOADING screen all render correctly.
- Next: press A+B at the title, follow menus into a match; check audio clipping; controls.

## 2026-10-26 midday
- Reaches title, menus' LOADING screens and attract-mode 3D gameplay (HUD, players, stadiums).
- KNOWN BUG (desktop window run): pressing to start during the intro movie froze the game
  (main thread 100% CPU, guest spin). Not reproducible headless with scripted presses — suspect the
  live-input / SDL window / audio path. Debug: `kill -USR1 <pid>` prints a guest thread dump.
- Renderer: 3D field + stadium background render black; EFB copies now format-converted (shadows).

## 2026-10-26 afternoon
- Fixed: GX front-face winding (sky domes / stadium geometry were culled) -> attract-mode stadiums
  now render correctly (Yoshi Park, city stadium: sky, grass, shadows, logos). Screenshot:
  build/latest_yoshi_park.png
- Desktop "crash": likely the window stopped responding while no frames were presented; window
  events are now pumped every retrace. Still unconfirmed — retry on the desktop.
- Next: navigate title -> menus -> exhibition match with scripted input; then controls/swing mapping.
