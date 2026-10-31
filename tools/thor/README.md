# tools/thor: testing on the AYN Thor

Shell/Python helpers for test runs on the Thor over USB (Windows adb at
`~/laar-build/android/win/platform-tools/adb.exe`, serial `<serial>`). Outputs go to
`$LAA_OUT` (default `~/laar-build/thor-out`); image/video analysis uses `~/laar-build/venv`
(Pillow, NumPy, OpenCV: `python3 -m venv ~/laar-build/venv && ~/laar-build/venv/bin/pip install pillow numpy opencv-python-headless`).

Test runs write their own `env.txt` and use the test saves (`LAA_SAVE_DIR=files/testsave`, a copy of
the player's saves in Mabe Village; `files/testsave2` is the beach with the sword). At the end they put back
the player's play settings from `C:\Users\you\laa_env_play.txt`.

| script | what it does |
|---|---|
| `deploy.sh` | build libmain.so + APK and install it; build the game library (libgame-<id>.so) and push it to files/game/ when it changed (run from the repo root) |
| `push_game.sh [build dir]` | strip a game library and push it to files/game/ when it changed (deploy.sh does build/game; e.g. `~/laar-build/android/build/game101` for 1.0.1) |
| `thor_run.sh out.txt [KEY=VAL...]` | start the game, script the input (`SCRIPT=...`, default: load the autosave), run to frame 3600, pull log.txt and the frame-3000 screenshot (`SHOT_EVERY=n`) |
| `thor_power.sh tag [KEY=VAL...]` | thor_run + from frame 1800 for 30 s: average CPU cluster / GPU clocks, GPU busy, battery/usb current, temps, slow frames (`pw_report.py`) |
| `thor_heat.sh`, `gpuload.sh tag` | GPU busy x clock (real GPU work, Mcycles/s), temps, per-thread CPU (`threads.py`) |
| `thor_perf.sh prefix frame secs` | simpleperf (frame pointers) over a window of a run |
| `watch_play_env.sh tag [KEY=VAL...]` | the player plays: waits for the app, screenrecords in 3-min pieces, captures log.txt + logcat (it notices the app closing only between pieces) |
| `beach_hang.sh n` | repeat the beach script (`beach_script.txt`, testsave2) until dmesg shows a new GPU hang |
| `frames.py log` | per-frame summary of an LAA_TRACE_SYNC log (slow frames and what was in them) |
| `passes.py log` | a frame's passes/copies/clears in order from an LAA_TRACE_PIPELINES log |
| `fade.sh`, `pullshots.sh`, `wavstats.py` | load-in brightness, pull+shrink screenshots, WAV loudness |

thor_run.sh appends the prime core's average clock over the run ("thor_run: prime core (cpu7) average ... GHz"):
the governor keeps it anywhere from 1.84 to 3.19 GHz from one session to the next, and the ms per frame scale with
it, so compare only runs made back to back (A/B/A) or at the same clock.

`walk.txt` is a walking input script for Mabe Village (`SCRIPT="$(cat walk.txt)"`). Pass
`LAA_GPU_TIMING_AT=0` when counting slow frames: thor_run's default times frame 3000, which waits on the GPU.
