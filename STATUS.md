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
- FIX: Wii Remote pointer works (mouse in the window). KPAD rejected our sensor-bar dots: this
  game caps the remote distance at ~1 m, so the dots are now 360 px apart, and the y offset
  cancels KPAD's sensor-bar-below-TV correction. Centre reads (0,0), corners about +-1. The P1
  hand cursor shows and highlights menu items. Contact sheet: build/pointer_works.png
- Scripted runs keep the pointer off-screen unless RT_IR="x,y" is set (d-pad navigation).
- Next: pitching gesture, swing timing, desktop input test.
- Pitching: holding the remote pointing up (`P`) satisfies the "Pull up" prompt (Mario winds up,
  prompt becomes "Throw"). A sideways swing does not count as a throw; testing an overhand throw
  profile (`T` / `Y`, either sign of the forward axis).

## 2026-10-26 afternoon
- FIX (speed): the match was pinned at exactly 30 fps. The main loop waits one retrace per frame and
  frame work ran just over 16.7 ms, so every frame slipped to two retraces. getenv on hot paths,
  libm ldexp/nearbyint and fat GL vertices (156 -> 108 bytes) fixed it: matches hold ~60 fps.
- FIX (render): scrambled/grey character icons (batting order, roster). The per-frame texture
  fast path ignored palette (TLUT) reloads into the same TMEM slot.
- RT_VIDEO=file.mp4 streams frames to ffmpeg in wall-clock time (true playback speed); mux RT_WAV
  with the printed offset. Demo: build/mss_linux_demo2.mp4
- Tools: per-thread host profiler with library/caller attribution; RT_WATCH=addr:len:tick data
  watchpoint (tools/watch.py) - the game reads motion via fn_8050C034 / fn_8050C0B8.
- Pitching: "Pull up" = a quick upward jerk (S does it). "Throw: swing down" still not reliably
  detected (one pitch registered by chance); T is now a swing-down profile, untested in pitching.
  Practice can be skipped with + (pause in a match; A resumes).
- Next: pin down the throw detector (callers of fn_8050C034: fn_801CB44C, fn_8024A69C,
  fn_805163F8), then a desktop play test.

## 2026-10-26 evening
- MILESTONE: pitching works -> full innings play out: Mario winds up and throws, CPU batters hit,
  fielders make outs, CHANGE SIDES, next inning (build/pitching_works.png, from build/v14.mp4).
- How: the throw is matched by fn_801649B0: in the motion-direction history (fn_805163F8 buckets
  the normalised KPAD acc into 26 directions), an earlier segment must have KPAD z- or y+ and a later
  one z+ without y+ (patterns at 0x80643770, axis bitmask table 0x80628398). T now produces that.
- Desktop keys: Space = swing / pull up, C = throw.
- Next: desktop play test; check the occasional black frame in recordings; renderer vs Dolphin.
