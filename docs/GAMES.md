# Games: checks, contention and moving work off the game's GPU (1.14)

GPU Fusion cannot split a game's frames across the GT 730 and the HD Graphics 2000 (see
[GAME_MULTI_GPU_FEASIBILITY.md](GAME_MULTI_GPU_FEASIBILITY.md)). What it can do for **any** game,
without touching the game, is:

- find what else uses the game's GPU;
- move what can be moved to the other GPU, with your approval;
- measure whether that helped.

This works the same for every game and program; nothing in it is specific to one title. Labels:

- **[Test]**: measured on the reference PC on 2026-10-28.
- **[Est.]**: estimated from Windows' counters.

## The Games page (and `--game-check`)

**Steps:**

1. Start the game, then on **Games** click *Refresh list*. It lists every program using a GPU right
   now, the busiest first.
2. Pick the game and click *Check*.
3. For the chosen time (default 20 s), GPU Fusion samples once a second:
   - every process's GPU engine use on every GPU [Est.]: Windows GPU performance counters, and
     D3DKMT statistics for the WDDM 1.2 HD 2000;
   - each CPU core's load and the game's own CPU time [Est.];
   - optionally the game's **frame times** [measured]: Windows graphics events, the administrator
     prompt, see [FRAME_TIMES.md](FRAME_TIMES.md).

**The result shows:**

- the game's GPU and how busy the game kept it;
- every **competitor**: another program on the game's GPU, with its share, and whether it can move to
  the other GPU and how. That comes from the [compatibility registry](COMPATIBILITY_REGISTRY.md).
- **indications**, labelled as indications:
  - *GPU-limited*: the game's busiest engine at 85 % or more on average;
  - *other programs use the game's GPU*: competitors at 5 % or more combined;
  - *CPU-limited*: the GPU under 70 % while the game used 85 % or more of one core's time, or the
    busiest core averaged 90 % or more;
  - otherwise *no clear limit* (for example vertical sync or a frame-rate cap).

  Utilization shows where time goes, not what limits the frame rate. A before/after frame-time
  measurement is what shows whether a change helped.
- a saved check (`<data>\games\`) and an HTML report (`<data>\reports\`).

**Before/after:** give the first check a label such as "before", change something, check again and
choose the first check under *Compare with*. When both checks measured frame times, the verdict is
GPU Fusion's significance rule on per-second frame times.

**Command line:**
`GPUFusion.exe --game-check <pid|exe> [--game-seconds N] [--game-frames] [--game-label before] [--game-compare last|<id>]`.

**What a check never does:** start, close or restart a program, or change a setting.

### Moving a competitor (the only change GPU Fusion makes here)

The program must have a known way to run on the Intel GPU (registry status Supported or
Experimental), for example Chromium's `--use-adapter-luid` for Edge, Brave and other Chromium browsers.

- **What GPU Fusion does:** it creates a **profile variant** ("Intel GPU (game check)") that starts the
  program on the Intel GPU. You confirm before it is created.
- **What it leaves alone:**
  - your own profiles, since a variant is separate;
  - Windows' GPU preference, which variants never write;
  - the running program, which GPU Fusion does not close or restart. You start it from the GPU
    Manager when you are ready.
- **Undo:** "Undo variants created here" removes exactly the variants listed in
  `<data>\config\game-check-variants.json`, and only if they still carry that variant name.

## Measured: a real background program on the game's GPU, then on the Intel GPU [Test]

- **Stand-in game:** `GPUFusionFrameTest --api d3d11 --load 256`, a Direct3D 11 window limited by the
  GT 730 at about 50 fps.
- **Background program:** a real application, a temporary Microsoft Edge instance started by the
  script with its own profile folder (never the user's browser), drawing a WebGL page:
  - first on its default GPU (the GT 730);
  - then started with `--use-adapter-luid` for the Intel GPU, the mechanism GPU Fusion verified earlier.
- **Method:** frame times measured by one 272 s capture, with two rounds of 30 s phases.
  Script: `scripts/experiments/frames_with_edge.py`; data:
  [data/frames-edge-2026-10-28.json](data/frames-edge-2026-10-28.json).

| Phase | Round 1 | Round 2 | Per-second means, both rounds |
|---|---|---|---|
| Game alone | 50.2 fps, median 19.5 ms | 49.7 fps, median 19.5 ms | 20.03 ms (49.9 fps) |
| Edge drawing on the **GT 730** | 27.6 fps, median 38.2 ms | 27.8 fps, median 37.8 ms | 36.14 ms (27.7 fps) |
| The same Edge started on the **Intel GPU** | 43.1 fps, median 22.5 ms | 43.1 fps, median 22.7 ms | 23.30 ms (42.9 fps) |

- **Moving Edge from the GT 730 to the Intel GPU made the game 1.55× faster** (27.7 → 42.9 fps; the
  significance rule says "faster", 56 one-second samples each).
- **Not all of the loss comes back.** With Edge on the Intel GPU the game stays slower than alone
  (42.9 vs 49.9 fps). Edge's window still has to be composited by Windows on the GT 730, which drives
  the display, and its frames copied across from the Intel GPU. That remaining cost is the likely
  cause; it was not measured on its own.
- The game check of the same situation [Est.] saw:
  - Edge alone at 22 % of the GT 730 (busiest engine), and the game's share falling from 99 % to 75 %;
  - with Edge on the Intel GPU, Edge gone from the GT 730's list.
- **Real games:** none was started for these measurements (GPU Fusion does not start your games). Any
  game can be checked the same way while you play it.

Earlier, with a synthetic background program (`scripts/experiments/game_contention.py`):

- a continuously rendering program on the GT 730 cost a GPU-bound program 77 % more time per frame,
  and on the Intel GPU nothing;
- H.264 video decoding cost nothing on either GPU, because the decoder is a separate engine.

## Overhead

- **A check's sampling:** less than 10 % of all cores is required by the test; in practice it is a
  few tenths of a percent (`checkCpuPercent` in each saved check).
- **Frame-time capture:** see [FRAME_TIMES.md](FRAME_TIMES.md).
- **Nothing runs in the background** when no check is running: the Games page samples only on
  *Refresh list* and *Check*.

## Limits

- **Utilization is an estimate**:
  - the busiest engine per process, sampled once a second;
  - the busiest engines of different processes can add up to more than 100 %;
  - protected processes are not readable through D3DKMT, and the report says how many.
- **Only programs with a known way to run on the Intel GPU can be moved.** Today that means Chromium
  browsers (verified for Edge and Brave), Electron apps (Experimental), FFmpeg, and programs that
  integrate the GPU Fusion SDK. Windows' own preference cannot select the Intel GPU on this PC.
  OpenGL programs follow the display GPU.
- **Windows' compositor (`dwm.exe`) cannot be moved:** it always works on the display GPU, and its load
  grows with what changes on screen.
- **Moving a program costs a little:** a visible window rendered on the Intel GPU still costs the
  display GPU some work, as measured above.
