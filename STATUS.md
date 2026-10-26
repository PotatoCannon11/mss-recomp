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
- Audio clipping check: done. The intro movie's level and clipping match Dolphin's DSP dump
  (RMS 7214 vs 7247 / 8040 vs 7834, clipped samples 3/6 vs 3/5 per 5 s); Dolphin clips about
  100 samples per 5 s on the title music too, so the hot mix is the game's own, not the AX mixer's.
- Fades to black between plays (~1 s) are the game's transitions, not dropped frames.
- Renderer vs Dolphin: drove Dolphin's emulated remote with XTest (tools/xkey.c; keys bound in
  WiimoteNew.ini, BackgroundInput on, configs restored afterwards) and grabbed its window from the
  private X display. In the pitching-practice demo at Mario Stadium the render matches: grass,
  dirt, shadows, wall, HUD, dialog. The visible difference was the aspect ratio: Dolphin's
  SYSCONF is widescreen.
- Widescreen by default: SYSCONF IPL.AR = 16:9 (patched into existing NANDs), window and RT_VIDEO
  present at 16:9; the game switches to its 16:9 layouts and wider camera. RT_43=1 for 4:3.
- RT_VIDEO stretches the 448-line image to the full frame like the VI (no black strip).
- Clock: the Wii clock is local time (was UTC, so save times were 4 h off here).
- Next: desktop play test (mouse pointer, Space/C gestures), more scene comparisons vs Dolphin.
- Windowed test on the private display (SDL window, XTest input): clicking A during the intro
  movie skips to the title and the game keeps running at 60 fps, so the earlier desktop freeze did
  not reproduce. A+B (both mouse buttons) works, and click-to-select works.
- Open: the menu hand cursor follows the mouse for the first couple of moves at file select, then
  stays put, although KPAD's pointer keeps tracking (pos and dpd_valid=2 in RT_PADLOG). The game's
  own cursor logic (KPAD pos is read via fn_8050C15C) needs a look. The d-pad works meanwhile.

## 2026-10-26 night (2)
- Pointer re-checked: NOT a bug. Over 32 mouse glides in the SDL window, KPAD's pointer tracked
  every move (dpd_valid=2 throughout) and the P1 star moved between cards. Dolphin behaves the same
  with the same XTest moves. The "stuck" frames last time were grabs taken mid-animation.
- RT_INPUT gains `@x,y` (point at x,y in -1..1 for that tick range) for scripted pointer menus.
- State: boot -> menus (pointer or d-pad) -> exhibition match with batting + pitching at 60 fps,
  16:9, audio matching Dolphin. Controls: mouse = pointer, left/right click = A/B, Space = swing /
  pull up, C = throw, arrows = d-pad, Enter = +.
- Next: fielding/baserunning controls check, more Dolphin scene comparisons (other stadiums,
  minigames), then cleanup toward readable C/modding.

## 2026-10-26 late
- Second stadium: Yoshi Park (stage select, Right) loads and plays a full exhibition: intro,
  pitching practice, live pitching with outs, fielding cameras, CPU batting. Render clean at
  full resolution (mound texture, glow rings, HUD transparency, shadows). build/yoshi_park.png
- Fielding works in practice: hits get fielded and outs recorded (the remote stays at rest).
- Next: try the remaining unlocked parks and a minigame; check baserunning input (B/+?) in a
  batting half; compare one Yoshi Park frame against Dolphin.
- Minigame: Gem Catch plays start to finish (Minigames -> stage -> Yoshi -> Easy -> Start):
  Wario intro, gem cannon, catches and combos, "Clear!" at 1150/1000 points. build/minigame_gem_catch.png
  (The rules pop-up closes with B, not A, on its last page: game design, not a bug.)
- Next: baserunning input in a batting half; Challenge mode entry; desktop play test.
