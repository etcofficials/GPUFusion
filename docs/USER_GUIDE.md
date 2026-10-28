# GPU Fusion — user guide

How to do the common things, step by step. Background and measurements are in
[GPU_SELECTION.md](GPU_SELECTION.md) and [MULTI_GPU_RESEARCH.md](MULTI_GPU_RESEARCH.md); problems are in
[TROUBLESHOOTING.md](TROUBLESHOOTING.md).

**Contents:**
1. [Choose the GPU for a program](#1-choose-the-gpu-for-a-program)
2. [Run a program on a GPU Windows will not give it](#2-run-a-program-on-a-gpu-windows-will-not-give-it)
3. [Start it that way every day: shortcuts](#3-start-it-that-way-every-day-shortcuts)
4. [Check that it works](#4-check-that-it-works)
5. [One program on two GPUs: variants](#5-one-program-on-two-gpus-variants)
6. [Back up or move your profiles](#6-back-up-or-move-your-profiles)
7. [Run a batch of jobs on both GPUs](#7-run-a-batch-of-jobs-on-both-gpus)
8. [What GPU Fusion never does](#8-what-gpu-fusion-never-does)

---

## 1. Choose the GPU for a program

1. Open **GPU Manager**, then **Add program…** to pick a program from the Start menu, the installed
   programs or the running ones. Or use **Browse…** to choose its `.exe`.
2. Choose a mode: **Automatic**, **Integrated GPU**, **Discrete GPU** or **Both GPUs (SDK programs)**. Each mode shows
   its status *for this program on this PC*:

   | Status | Meaning |
   |---|---|
   | Verified | shown to work on this PC (by the probe test, or by verifying this program) |
   | Expected (not verified yet) | the mechanism should work; use *Verify on this PC* to confirm |
   | Only with the program's support | Windows cannot do it here; the program has to choose the GPU itself (see section 2) |
   | Not possible on this PC | e.g. only one GPU, or no Direct3D 12 for splitting one rendering workload |

3. Press **Save and apply**. If a Windows setting will change, GPU Fusion shows exactly which value,
   from what to what, and asks first. Every change is journalled and can be undone on the **GPU
   Preferences** page.

Windows' per-app preference takes effect **when the program next starts**. A program that is already
running keeps its GPU.

## 2. Run a program on a GPU Windows will not give it

On some PCs (including the reference PC) Windows gives every program the same GPU, whatever the
preference says. Many programs have their **own** option for choosing a GPU. GPU Fusion can pass it:

1. Select the program in the GPU Manager. If GPU Fusion knows its option, it shows **Known GPU
   option…** with the evidence behind it. Press **Use this option**.
2. Otherwise, type the program's option into **Launch arguments** with a placeholder. GPU Fusion fills
   the placeholder in for the GPU of the chosen mode every time it starts the program:

   | Placeholder | Example |
   |---|---|
   | `{luid_pair}` | `0,34575` (Chromium browsers: `--use-adapter-luid={luid_pair}`) |
   | `{dxgi_index}` | `1` (FFmpeg: `-init_hw_device d3d11va=gf:{dxgi_index} -hwaccel d3d11va -hwaccel_device gf`) |
   | `{luid}`, `{luid_high}`, `{luid_low}`, `{vendor_id}`, `{vendor_hex}`, `{gpu_name}` | other forms programs use |

3. **Save and apply**, then **Verify on this PC**.

This works only when the program is started **through GPU Fusion**: *Launch*, *Verify*, a GPU Fusion
shortcut (section 3) or `--launch-profile`. Browsers ignore the option if they are already running,
because the new window joins the running browser. GPU Fusion offers to close the running instance
first.

Programs without such an option, and without the [SDK](SDK.md), cannot be moved to a GPU that Windows
will not give them. GPU Fusion says so instead of pretending.

## 3. Start it that way every day: shortcuts

**Create shortcut…** makes a shortcut such as `msedge (Integrated GPU).lnk`, with the program's own
icon. You choose where it goes; the Desktop is the default. Opening it starts the program through its
profile **without opening GPU Fusion's window**. If the program is already running, a small dialog
asks: *Close it and start*, *Start anyway* or *Cancel*. Later changes to the profile apply to the
shortcut automatically.

## 4. Check that it works

- **Verify on this PC** (GPU Manager) starts the program and watches its processes for a while. Use
  the program normally meanwhile, and keep a browser window visible, because browsers pause drawing
  when covered. The result says which GPU(s) it actually used, in four stages. For the Intel HD 2000
  the result says "work of unknown type", because its driver does not name its engines: GPU Fusion
  can say *where* the work ran, not that it was 3D.
- **Applications** page, **GPU profile** column: for programs that are running anyway, it shows live
  whether each one is on the GPU its profile asks for. ✓ means yes. ✗ means it is on another GPU; the
  tooltip says which and why, usually because it was not started through GPU Fusion. *idle* means
  there is nothing to compare right now. Nothing is launched for this.

### Measure a program's frame times

On the **Applications** page, select a running program and click **Measure frame times…**.

- **What happens:** GPU Fusion records when the program presents each frame, from Windows' own
  graphics events. Nothing is loaded into the program, so this is safe with games that use
  anti-cheat software.
- **Permission:** Windows asks for administrator rights for each measurement. Approve it to measure;
  decline it and nothing is measured. No setting is changed either way.
- **The result:** frames per second, median and 99th-percentile frame time, "1 % low" and stutters
  (measured), plus the program's GPU use (estimated, from GPU Fusion's monitoring).
- **Before and after:** give a measurement a label such as "before", change something, measure again
  as "after", and choose the first one under *Compare with*. The verdict says "faster" or "slower"
  only when the difference is real.
- **Keep the program on screen and drawing:** a minimized or paused game draws no frames.

Details, accuracy and limits: [FRAME_TIMES.md](FRAME_TIMES.md).

## 5. One program on two GPUs: variants

**Add variant…** creates another profile for the same program, e.g. "FFmpeg — Intel" and
"FFmpeg — NVIDIA". Each variant has its own mode, arguments and shortcut. Windows stores only **one**
GPU preference per program, and it belongs to the main profile, so a variant reaches its GPU through
the program's own option (section 2) or the SDK. For browsers, give each variant its own
`--user-data-dir`, otherwise they join one running browser.

## 6. Back up or move your profiles

**Export…** saves all profiles to one file. **Import…** adds them back, on this PC or another one.
Existing profiles are kept unless you choose to replace them. Importing changes no Windows setting:
apply a profile to put it into effect. Verification results are not imported, because they describe
the PC they were made on.

For automatic backups, set a folder in **Settings → GPU profiles**, preferably on another drive.
GPU Fusion then saves the profiles there after every change and keeps the newest copies. To restore,
use **Import…** with the newest `gpufusion-profiles-….json` file.

## 7. Run a batch of jobs on both GPUs

**Workload Lab → Your own jobs on several GPUs** runs one job per input file, spread over the GPUs.
Use either a command template (any program that accepts a GPU option, e.g. the FFmpeg example shown
on the page) or the built-in H.264 decoder.
- **Run** does the batch once.
- **Compare** measures each GPU alone, the fastest GPU running the same number of jobs at once, and
  all GPUs. It checks that the outputs are identical, and calls a result faster or slower only when
  the difference is significant. On the reference PC the second GPU added at most about 10–15 %, and
  sometimes made a batch slower; the page reports whichever it measures.

## 8. What GPU Fusion never does

- Inject code into, patch or modify other programs.
- Change drivers, firmware/BIOS or system-wide graphics settings. The only setting it writes is a
  program's per-user Windows GPU preference, after you confirm, journalled and undoable.
- End a program by force. It asks programs to close, like clicking their close button.
- Claim that a program got faster because both GPUs show activity.
- Make one rendering workload run on two GPUs. That needs the program's own multi-GPU support, and
  on the reference PC it is impossible, because the Intel GPU has no Direct3D 12.

Command-line equivalents of everything above are listed in the [README](../README.md#running).

## Using both GPUs for one workload

**Both GPUs (SDK programs)** only *offers* both GPUs to a program. Programs that integrate the GPU
Fusion SDK divide their work between them; every other program, games such as Roblox or Minecraft
included, runs on one GPU as in Automatic. The Applications page then shows **One GPU only (…)**,
not a check mark.

To see what dividing one workload between these GPUs actually gives, use **Workload Lab → Adaptive
multi-GPU scheduler**. Choose a size and a policy, then click *Run experiment*. GPU Fusion:

1. measures each GPU alone;
2. lets the faster GPU (the GT 730) do most of the image while the Intel GPU helps with small batches,
   only as many as its measured speed allows;
3. checks every image bit for bit;
4. compares the result with the faster GPU alone.

If the split is not faster, the result says so, and the scheduler stops using the Intel GPU. The
same scheduler is in the SDK, for programs that want it. `GPUFusionSplitDemo.exe`, next to
`GPUFusion.exe`, is a small example program you can run, or give a Both profile.
