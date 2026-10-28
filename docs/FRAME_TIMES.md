# Frame-time measurement (1.13)

GPU Fusion measures the frame times of a running program **from outside**, with the graphics events
Windows itself records (Event Tracing for Windows, ETW). These are the same events Intel's
open-source PresentMon uses. Nothing is loaded into the program and nothing about it is changed. That
matters for games with anti-cheat software, and for any program GPU Fusion must not tamper with.

This is phase 1 of game support: before GPU Fusion changes anything for a game, it must be able to
show, with numbers, whether the change made the game smoother.

Every statement below carries a label:

- **[Test]**: measured on the reference PC (i5-2400, GT 730 on driver 391.35, HD Graphics 2000;
  Windows 10 19045) on 2026-10-28.
- **[Source]**: from Windows' own event definitions (read with `wevtutil gp … /ge`).
- **[Not tested]**: expected to work, not yet shown.

## Using it

- **Applications page:** select a program, then click **Measure frame times…**. Choose the length,
  optionally a label such as "before" or "after", and optionally an earlier measurement of the same
  program to compare with. Then click **Measure**.
- **Command line:**
  `GPUFusion.exe --measure-frames <pid|exe> [--frames-seconds 20] [--frames-label after] [--frames-compare last|<id>]`.
  Exit code: 0 = frames measured; 10 = not measured (for example, the prompt was declined); 11 = the
  program presented no frames.
- **Results:** each measurement is saved under `<data>\frames\` and a readable HTML report under
  `<data>\reports\`. The report states what was measured, what was estimated and what was not
  measured, plus the before/after verdict.

**Permissions.** Windows lets only administrators, or members of the "Performance Log Users" group,
start an event trace.

- GPU Fusion therefore starts a small helper, `GPUFusionFrameCapture.exe`, through the **Windows
  administrator prompt each time** you measure. Approve it and the helper runs for that measurement
  only.
- Decline it and nothing is measured; GPU Fusion says so.
- GPU Fusion never adds you to a group and never changes a setting to avoid the prompt.
- If GPU Fusion itself already runs as administrator, there is no prompt.

## What is measured, estimated, or unavailable

| | How | Status |
|---|---|---|
| **Frame time** (time between two presents of the program) | **Measured.** The program's own present calls, from Windows' graphics events: DXGI Present start/stop (Direct3D 10, 11 and 12), Direct3D 9 Present start/stop, and kernel present events (Blit, Flip, PresentHistory and others) for programs that present without those runtimes. That covers OpenGL, Vulkan, and DirectDraw-era programs. | measured |
| Frames per second, median, 95th and 99th percentile, longest frame, "1 % low", long frames (stutters: > 2 × median and > median + 4 ms) | computed from the frame times | measured |
| Time spent inside the Present call | DXGI and D3D9 start/stop events | measured (DXGI, D3D9) |
| Vertical sync (sync interval), presents of a hidden window, calls that presented nothing | from the DXGI event's fields | measured (DXGI) |
| **GPU use of the program** | **Estimated.** GPU Fusion's usual per-process monitoring, sampled once a second: Windows GPU performance counters (GT 730, WDDM 2.3) or D3DKMT statistics (HD 2000, WDDM 1.2). It reports the busiest engine. | estimated |
| When a frame reached the display, dropped frames, input latency, GPU time per frame, CPU time per frame | would need further events and analysis (PresentMon's display tracking); not implemented | **not available** |

**Before/after comparison.**

- **Samples:** the mean frame time of each whole second. Consecutive frame times are not independent,
  so they are not compared one by one.
- **Rule:** the same as everywhere in GPU Fusion. "Faster" or "slower" only when the difference is
  more than twice the combined standard error and at least 1 %.

## Accuracy: checked against ground truth [Test]

`GPUFusionFrameTest.exe` is a small window that presents with one API at a known pace and records the
time of each of its own present calls. The capture of that process is compared with what the program
recorded itself (`scripts/experiments/validate_frame_capture.py`; raw results in
[data/frame-capture-validation-2026-10-28.json](data/frame-capture-validation-2026-10-28.json)). Each
case is a 6 s capture of a program running for 9 s.

| API, GPU, pace | Program's own median | Captured median | Captured presents per s (program's own) | Notes |
|---|---|---|---|---|
| Direct3D 11, GT 730, 10 ms | 10.000 ms | 10.000 ms (DXGI) | 100.7 (100.0) | the program's one 27.017 ms hitch was captured as 27.017 ms |
| Direct3D 12, GT 730, 10 ms | 10.000 ms | 9.996 ms (DXGI) | 100.7 (100.0) | |
| Direct3D 9, GT 730, 10 ms | 10.000 ms | 10.000 ms (D3D9) | 100.8 (100.0) | longest 11.86 ms (program: 11.82) |
| OpenGL 4.6, GT 730, 10 ms | 10.000 ms | 9.995 ms (kernel presents) | 100.8 (100.0) | |
| Direct3D 11, **HD 2000**, 10 ms | 10.000 ms | 9.998 ms (DXGI) | 101.0 (100.0) | rendered on Intel, shown on the GT 730's display |
| Direct3D 11, GT 730, uncapped | 0.169 ms | 0.172 ms | 5342 (5410) | 129,361 events in 6 s, none lost |
| OpenGL, GT 730, uncapped | 0.253 ms | 0.254 ms | 3633 (3578) | |
| Direct3D 9, uncapped | 0.250 ms | 0.251 ms | 3865 (3847) | |
| Direct3D 11, vsync | 16.661 ms | 16.661 ms | 60.3 (60.3) | sync interval 1 reported |
| OpenGL, vsync | 16.659 ms | 16.659 ms | 60.7 (60.2) | |
| Direct3D 11, 60 fps with a 50 ms stall every 30 frames | 16.667 ms | 16.667 ms | 55.0 (54.7) | 11 stutters found, longest 66.672 ms (program: 66.667) |

- No events were lost in any case.
- The "per s" columns compare a nominal 6 s capture with the program's whole 9 s, so they agree to
  about 1 %.
- A DXGI or D3D9 program's frames also appear as kernel presents (the driver's side of the same
  present). GPU Fusion counts them once and says so in the report.
- **GPU Fusion end to end:** `--measure-frames` on a 60 fps test window, with no administrator prompt
  (GPU Fusion was already elevated), gave:
  - 60.0 fps, median 16.67 ms;
  - estimated GPU use 2 % mean / 3 % max on the GT 730's 3D engine;
  - exit code 0, with the session and report saved.

## Overhead [Test]

| | Result |
|---|---|
| Capture helper | 0.05–0.1 % of all CPU cores, 3.9–6.1 MB peak memory |
| GPU Fusion while measuring (GPU estimate, once a second) | 0.15 % of all cores |
| Events delivered | 4–6 per frame (Windows filters the rest at the source; event-ID filtering is accepted by all three providers) |
| Effect on the measured program, worst case | an uncapped Direct3D 11 program at about 5,500 presents per second: 5,510 → 5,276 presents/s while captured (−4.3 %, median of 3 alternating runs) |

The worst case works out to roughly 8 µs per present, spent writing the events inside the program's
own present calls. At normal game frame rates that would be about 0.05 % at 60 fps and 0.12 % at
144 fps; this is an extrapolation, not a measurement.

## Which programs can be measured

| | Status |
|---|---|
| Direct3D 10/11/12 (DXGI) programs, any GPU | measured [Test] on the GT 730 and the HD 2000 (Direct3D 12 on the GT 730 only; the HD 2000 has none) |
| Direct3D 9 programs | measured [Test] |
| OpenGL programs | measured [Test] through kernel presents. Minecraft Java Edition renders with OpenGL 3.3 on the GT 730 here (its own log); expected to be measurable [Not tested]. |
| Vulkan programs | the same kernel path as OpenGL, as in PresentMon [Not tested]: neither GPU here has a Vulkan driver |
| Roblox (Direct3D 11, Store package, anti-tamper) | expected to be measurable: the capture never touches the process [Not tested] |
| Windows itself | `dwm.exe` (the compositor, DXGI) and `csrss.exe` (kernel presents) were seen when nothing else was drawing [Test] |
| Programs that draw nothing while measured (minimized, idle, paused) | reported as "presented no frames", never as 0 fps |

**Not tested with real games yet.** No game was started for this phase: GPU Fusion does not launch
your programs without being asked. The first real-game measurements belong to phase 2.

## Limits

- Windows' graphics events record when a program **presents** a frame, not when the display **shows**
  it. Display times, dropped frames and latency are not measured.
- Each measurement needs administrator rights (see Permissions).
- Only one capture runs at a time (one trace session, named "GPU Fusion Frame Capture"). A session
  left behind by a crashed capture is stopped by the next one.
- Event IDs are taken from Windows 10 19041's providers, and PresentMon uses the same IDs. A future
  Windows that changes them would show up as "presented no frames" for programs that do present.
- Long captures of very high frame rates store at most 500,000 presents per swap chain.

## How phase 2 builds on this

Phase 2 is general application discovery and GPU workload management.

- **Before/after is the gate.** Any change GPU Fusion offers is measured with this tool before and
  after, on the game the user cares about, and reported with the same verdict rule. Examples:
  - moving a program off the GT 730;
  - a different GPU option for an application.
- **Discovery.** An all-process capture shows which programs are *drawing frames* right now, and
  through which API (DXGI, D3D9 or kernel). With GPU Fusion's per-process engine monitoring it shows
  which programs keep a GPU busy while the game runs. Those are the candidates for capability C,
  moving other programs' GPU work away from the game's GPU.
- **Kept separate:** measured frame times (this tool), estimated GPU use (performance counters), and
  what an application *can* do (its documented options), as the phase-2 requirements ask. These
  numbers measure only a program's own frames; they do not show capability A, splitting a frame
  across GPUs.
