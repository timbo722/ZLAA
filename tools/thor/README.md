# tools/thor: testing on the AYN Thor

Shell/Python helpers for test runs on the Thor over USB. Copy `local.sh.example` to `local.sh` (kept out of git) and
set adb (on WSL the Windows `adb.exe`, the only one that sees USB devices), the device's serial when several are
connected, and a local folder for files on their way to or from the device (`env/`, `transfer/`, `game/`, `apk/`; on
a Windows drive when adb is `adb.exe`); `env.sh` reads it. Outputs go to
`$LAA_OUT` (default `~/laar-build/thor-out`); image/video analysis uses `~/laar-build/venv`
(Pillow, NumPy, OpenCV: `python3 -m venv ~/laar-build/venv && ~/laar-build/venv/bin/pip install pillow numpy opencv-python-headless`).

Test runs write their own `env.txt` and use the test saves (`LAA_SAVE_DIR=files/testsave`, a copy of
the player's saves in Mabe Village; `files/testsave2` is the beach with the sword). At the end they put back
the player's own settings from `env/laa_env_play.txt` in that folder.

| script | what it does |
|---|---|
| `deploy.sh` | build libmain.so + APK and install it; build the game library (libgame-<id>.so) and push it to files/game/ when it changed (run from the repo root) |
| `push_game.sh [build dir]` | strip a game library and push it to files/game/ when it changed (deploy.sh does build/game; e.g. `~/laar-build/android/build/game101` for 1.0.1) |
| `thor_run.sh out.txt [KEY=VAL...]` | start the game, script the input (`SCRIPT=...`, default: load the autosave), run to frame 3600, pull log.txt and the frame-3000 screenshot (`SHOT_EVERY=n`) |
| `thor_power.sh tag [KEY=VAL...]` | thor_run + from frame 1800 for 30 s: average CPU cluster / GPU clocks, GPU busy, battery/usb current, temps, slow frames (`pw_report.py`) |
| `thor_heat.sh`, `gpuload.sh tag` | GPU busy x clock (real GPU work, Mcycles/s), temps, per-thread CPU (`threads.py`) |
| `thor_perf.sh prefix frame secs` | simpleperf (frame pointers) over a window of a run |
| `watch_play_env.sh tag [KEY=VAL...]` | the player plays: waits for the app, screenrecords in 3-min pieces, captures log.txt + logcat (it notices the app closing only between pieces) |
| `guard_hunt.sh n [KEY=VAL...]` | repeat the beach script with LAA_SHADER_GUARD=1 until a shader reports hitting the translator's dispatch guard (which shader used to hang the GPU); a new kernel GPU hang's report is saved as guard<run>_dmesg.txt |
| `ab_game.sh tag rounds libA libB` | two game libraries (files in the local folder's `game/`) alternated A B A B on the village walk: CPU cycles in the game library and the runtime (simpleperf, clock-independent), the prime core's clock, frames over 20 ms; leaves B installed |
| `flicker.sh tag [KEY=VAL...]` | the grass walk east of the house, every frame 2600-2700 captured, flicker measured (`flicker_energy.py dir first last [sigma] [--panel]`; --panel: frames scaled to the 1920x1080 panel first, to compare 720p with 1080p) |
| `menu_test.sh outdir [s]` | the menus without hands (LAA_UI_SCRIPT): start screen, settings, Play, the pause menu at 45 s, settings, Resume; menu frames as PNGs + a screencap at the end; test save; play env put back |
| `background_test.sh prefix` | HOME in the middle of the game and back, twice: the game must stop while hidden and go on, on a new window surface, after (screencaps of the panel) |
| `beach_hang.sh n` | repeat the beach script (`beach_script.txt`, testsave2) until dmesg shows a new GPU hang |
| `frames.py log` | per-frame summary of an LAA_TRACE_SYNC log (slow frames and what was in them) |
| `passes.py log` | a frame's passes/copies/clears in order from an LAA_TRACE_PIPELINES log |
| `fade.sh`, `pullshots.sh`, `wavstats.py` | load-in brightness, pull+shrink screenshots, WAV loudness |

thor_run.sh appends the prime core's average clock over the run ("thor_run: prime core (cpu7) average ... GHz"):
the governor keeps it anywhere from 1.84 to 3.19 GHz from one session to the next, and the ms per frame scale with
it, so compare only runs made back to back (A/B/A) or at the same clock.

`walk.txt` is a walking input script for Mabe Village (`SCRIPT="$(cat walk.txt)"`). Pass
`LAA_GPU_TIMING_AT=0` when counting slow frames: thor_run's default times frame 3000, which waits on the GPU.
