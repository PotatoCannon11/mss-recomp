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

## 2026-10-26 evening
- MILESTONE: full menu flow into a match works (headless, scripted input): title -> file select ->
  menu -> stage select -> captain/roster -> batting order -> rules -> "Mario Fireballs vs Luigi
  Knights" loading -> in-stadium cutscene -> pitching tutorial. Contact sheet: build/match_reached.png
- Clock now counts from 2000-01-01 like the Wii (real dates in saves).
- Next: get through the tutorial into live pitching/batting; tune motion (swing/pitch) input.

## 2026-10-26 night
- Performance: 3D gameplay went from 12-19 fps to 30-48 fps (release build without tracing,
  texture/program hash caches, per-frame texture validation, uniform/state change skipping,
  draw batching into indexed triangle lists). Debug build: `make EXTRA=-DTRACE`.
- Live gameplay renders correctly with batching: PLAY BALL, pitches, hits, fielding cameras.
- Tools: RT_FPS, RT_THREADPROF (guest thread time), RT_HOSTPROF (+_FROM frame) + tools/hostprof.py.
- Next: confirm hardware frame rate in Dolphin; vertex decode speed; desktop input test.
- Live match plays: batting as Mario (Swing prompt, pitches, count, outs) -> CHANGE SIDES ->
  scoreboard -> pitching. Contact sheet: build/live_gameplay.png
- Open: does the swing gesture register (outs may be strikeouts)? which button skips tutorials?

## 2026-10-26 late night
- FIX: motion controls work. KPAD reported a frozen accelerometer because WPADGetDataFormat
  (unnamed fn_806022D0) read the SDK's unpopulated control block and returned "buttons only";
  now HLE'd along with the DPD-mode query (fn_80604A64). Swings register: batting practice
  "GOOD!", live at-bats swing. Contact sheet: build/swing_works.png
- Swing is a 13-sample acceleration profile (Space key, or `S` in RT_INPUT); `P`/`Q` in
  RT_INPUT hold the remote pointing up/down. RT_FRAMES_FROM/TO limit frame dumps; RT_PADLOG
  logs the game's KPADStatus.
- Next: check the IR pointer (KPAD pos still reads 0,0), pitching gesture, swing timing.
