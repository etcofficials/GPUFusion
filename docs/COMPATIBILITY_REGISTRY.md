# Compatibility registry (1.14)

What GPU Fusion can do with a program, answered **separately** for six capabilities. They are easy to
confuse, so they are never merged into one "supports both GPUs":

| Key | Capability | Example |
|---|---|---|
| `split-frames` | Render one program's frames on both GPUs | D3D12 explicit multi-adapter. Impossible on the reference PC: the HD 2000 has no D3D12, neither GPU has Vulkan. |
| `split-workloads` | One program runs its own separate workloads on both GPUs | `GPUFusionImage`, the split demo, a Godot project using the GPU Fusion extension |
| `move-off-game-gpu` | Move this program's GPU work off the GPU a game uses | Edge started on the Intel GPU while a game runs on the GT 730 |
| `select-gpu` | Choose which GPU the program uses | Chromium's `--use-adapter-luid`, FFmpeg's `-init_hw_device` |
| `concurrent-jobs` | Independent jobs of the program on both GPUs at once | FFmpeg jobs through GPU Fusion's job dispatcher; `GPUFusionImage --mode batch` |
| `scheduling` | Faster through workload scheduling and resource management | the SDK's adaptive scheduler; for other programs: contention checks and frame-time measurement only |

**Statuses:**

- **Supported**: tested and verified.
- **Experimental**: implemented or documented, not yet validated.
- **Partially supported**: only some features or workloads.
- **Unsupported**: not possible through documented interfaces on this hardware.
- **Not tested**: not established.

## Where the answers come from

The answers are combined in this order, each later step overriding the earlier ones:

1. **The program's files.** GPU Fusion examines the executable without starting it:
   - **imported graphics APIs**: Direct3D 9/11/12, DXGI, OpenGL, Vulkan, CUDA, OpenCL;
   - **runtime and engine**:
     - Chromium (`icudtl.dat` plus a V8 snapshot);
     - Electron (Chromium plus `resources`);
     - Java (`javaw.exe`, `jvm.dll`);
     - Unity (`UnityPlayer.dll`);
     - Unreal Engine (`*-Win64-Shipping.exe`);
     - Godot (`godot*` or a `.pck`);
     - SDL, .NET, and the GPU Fusion SDK;
   - whether it is a Store/Xbox package.

   Programs that load their graphics API at run time (Chromium, Java, engines) count as graphics
   programs.
2. **This PC.** From GPU Fusion's adapter detection and preference verification:
   - which GPUs have Direct3D 12 or Vulkan;
   - which GPU is integrated;
   - whether Windows' per-app preference can select the integrated GPU (here it cannot).
3. **The generic classifier** combines the two:
   - a D3D12-only or Vulkan-only program cannot move to the HD 2000;
   - an OpenGL program follows the display GPU;
   - a Chromium program can use the switch;
   - an SDK program follows GPU Fusion's adapter list;
   - splitting frames needs D3D12 or Vulkan multi-adapter on every GPU;
   - and so on.
4. **Registry entries** (built in, plus yours). An entry matches executables or runtimes and sets
   statuses with reasons, a mechanism and its evidence. Your entries come first and override built-in
   ones.
5. **What GPU Fusion verified** for this very program: a confirmed verification of its profile in
   Integrated mode makes `select-gpu` and `move-off-game-gpu` Supported ("verified on this PC").

## Built-in entries

| Entry | Matches | Statuses it sets | Evidence |
|---|---|---|---|
| `gpufusion-sdk-tools` | GPUFusionSplitDemo, GPUFusionImage | split-workloads, concurrent-jobs, select-gpu, scheduling: Supported | measured 1.14× (hash, photos split), 1.15× (photos batch), bit-exact |
| `chromium-verified` | msedge.exe, brave.exe | select-gpu, move-off-game-gpu: Supported; concurrent-jobs: Unsupported | verified 2026-10-26; game frame rate +55 % when Edge moved (1.14) |
| `chromium-other` | chrome, vivaldi, opera, chromium | select-gpu, move-off-game-gpu: Experimental | same switch, not verified with these |
| `electron` | runtime Electron | select-gpu, move-off-game-gpu: Experimental | not verified (the Discord check was not done) |
| `ffmpeg` | ffmpeg.exe | select-gpu, move-off-game-gpu, concurrent-jobs: Supported | decoding verified on both GPUs; 8 jobs on both GPUs with identical outputs, but no faster than the HD 2000 alone with 2 jobs |
| `java` | javaw.exe | select-gpu: Partial; move-off-game-gpu: Unsupported; split-workloads: Not tested | Minecraft 26.3's own log: OpenGL 3.3 on the GT 730, Vulkan unavailable |
| `roblox` | RobloxPlayerBeta.exe | split-workloads, move-off-game-gpu, concurrent-jobs: Unsupported; select-gpu, scheduling: Partial | feasibility report |
| `windows-compositor` | dwm.exe, csrss.exe | all Unsupported | Windows composes on the display GPU |
| `obs` | obs64.exe | select-gpu, move-off-game-gpu: Experimental; split-workloads: Unsupported | OBS's own adapter setting; no usable hardware encoder here |
| `unreal` | runtime Unreal Engine | select-gpu: Experimental (`-graphicsadapter`) | documented option, not verified |
| `godot` | runtime Godot | select-gpu: Not tested (`--gpu-index` needs Vulkan/D3D12); split-workloads: Partial (image work through the GPU Fusion extension) | validated with Godot 4.7.2: 1.09–1.12× faster, bit-exact |

## Adding programs, engines and mechanisms

Put entries in `<data>\config\compatibility.json`. Invalid entries are reported and skipped, never
fatal. An example:

```json
[
  {
    "id": "my-renderer",
    "program": "My renderer",
    "executables": ["renderer.exe"],
    "status": { "select-gpu": "experimental", "move-off-game-gpu": "experimental" },
    "reasons": { "select-gpu": "Has --device <index> (documented in its manual)." },
    "mechanism": "--device {dxgi_index} in a GPU Fusion profile",
    "evidence": "Vendor documentation; not verified here."
  }
]
```

- **Keys:** capability keys as in the table at the top; statuses `supported`, `experimental`,
  `partial`, `unsupported`, `not-tested`.
- **Engine-wide entries** use `"runtimes": ["Unity"]` instead of executables.
- **A new mechanism:** a program's own GPU option goes in the profile's launch arguments with GPU
  placeholders (`{dxgi_index}`, `{luid_pair}`, ...). Adding it to `gpu-options.json` makes GPU Fusion
  suggest it.

## Using it

- **Command line:** `GPUFusion.exe --compatibility [exe ...]`.
  - Without arguments it assesses the programs with a GPU profile and the running programs that use a
    graphics API.
  - It writes an HTML table to the reports folder.
  - It reads files only and starts nothing.
- **Games page:** each competitor shows its `move-off-game-gpu` answer and mechanism.

**Assessed on the reference PC, 2026-10-28:**

- **Brave:** select and move are Supported.
- **Discord (Electron):** Experimental.
- **javaw (Minecraft):** select is Partial, move is Unsupported.
- **LumaCapture (Direct3D 11):** select is Partial, move Not tested; no known option.
- **Roblox:** move is Unsupported, select Partial.
- **SearchApp (DXGI):** select is Partial.
- **Edge, FFmpeg, GPUFusionImage:** as their entries.
- **Godot 4.7.2:** split-workloads Partial, through the extension; its validation is in
  [SDK.md](SDK.md).
