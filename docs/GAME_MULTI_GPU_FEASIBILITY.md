# Games and both GPUs: feasibility report (2026-10-28)

**Question.** Can games on the reference PC benefit from both the NVIDIA GeForce GT 730 and the Intel
HD Graphics 2000, through legitimate means? GPU Fusion uses no injection, no patching of executables,
no hooks and no process-memory access.

This report covers phases 1 to 3 of the request, plus a plan for what to build next. Nothing in it has
been implemented in GPU Fusion yet. Every statement carries one of these labels:

- **[Test]**: measured or read on this PC (2026-10-28).
- **[Log]**: read from the game's own log file on this PC.
- **[Source]**: from the cited documentation or reporting.
- **[Earlier]**: established in an earlier GPU Fusion phase (see [MULTI_GPU_RESEARCH.md](MULTI_GPU_RESEARCH.md)).
- **[Hypothesis]**: not yet tested; the experiment that would decide it is named.

## 1. The hardware facts that decide everything

| | GT 730 (Fermi GF108, driver 391.35) | HD Graphics 2000 (Sandy Bridge, driver 9.17.10.4459) |
|---|---|---|
| Direct3D 11 | feature level 11_0 [Earlier] | feature level 10_1, no compute shaders [Earlier] |
| Direct3D 12 | feature level 11_0 [Earlier] | **none**: `D3D12CreateDevice` fails [Earlier] |
| Vulkan | **none**: NVIDIA never shipped Vulkan for Fermi ([wccftech](https://wccftech.com/nvidia-fermi-geforce-400-500-vulkan-api-support/amp/), [Softpedia on driver 356.43](https://drivers.softpedia.com/blog/new-vulkan-nvidia-geforce-graphics-driver-is-available-version-356-43-beta-500884.shtml)) [Source]. No `vulkan-1.dll` on this PC [Log] | **none** [Source] |
| OpenGL | 3.3 core context given to Minecraft [Log] | 3.1 on Windows ([Wikipedia table](https://wikipedia.com/wiki/Intel_Extreme_Graphics), [Intel community](https://communities.intel.com/thread/37335)) [Source]. The OpenGL driver `ig7icd64.dll` is installed [Test] |
| OpenCL / CUDA | CUDA only with obsolete toolkits [Earlier] | no GPU OpenCL [Earlier] |
| Video | H.264 decode about 190 fps on the test clip, no NVENC [Earlier] | H.264 decode about 850 fps [Earlier]. **No Quick Sync encode runtime** in the installed driver (no `libmfxhw64`, no encoder MFT) [Test] |
| Speed on GPU Fusion's shader workload | 1× | about 1/5 [Earlier] |
| Sharing memory with the other GPU | none: different vendors, WDDM 1.2 on Intel. Data goes through system memory [Earlier] | same |

Two consequences follow:

- **No common modern API.** The only one shared by both GPUs is Direct3D 11 at feature level 10_1
  with pixel shaders, plus OpenGL 3.1 in theory.
- **No shared surfaces.** Every exchange between the GPUs is a copy through system memory. Reading
  back 4 MB from the GT 730 takes about 4 ms [Earlier].

## 2. The approaches, one by one

| # | Approach | API on both GPUs? | Game must change? | GPU Fusion alone, without touching the game? | Realistic here? | Possible gain | Overhead |
|---|---|---|---|---|---|---|---|
| 1 | **Split one game's rendering** (AFR/SFR, explicit multi-adapter) | **No.** It needs D3D12 cross-adapter heaps or Vulkan device groups on both GPUs ([Microsoft: shared heaps](https://learn.microsoft.com/en-us/windows/win32/direct3d12/shared-heaps), [multi-engine](https://learn.microsoft.com/windows/win32/direct3d12/multi-engine)) [Source]. The HD 2000 has neither. | yes, deeply ([Ashes of the Singularity was the showcase](https://www.anandtech.com/show/9740/directx-12-geforce-plus-radeon-mgpu-preview/2)) | no | **no** | none | not applicable |
| 1b | The same with D3D11 and copies through system memory | partly: D3D11 FL 10_1 only, and most games need FL 11_0 shaders | yes | no | **no** | Upper bound 1 / (1 + 1/5) ≈ 1.2× if perfect. In practice the frame round-trip (≥ 2 copies of 1–8 MB, plus a CPU sync per frame) costs more than the Intel GPU's sixth of the frame. | High CPU and transfer cost, added latency |
| 2 | **Game on the GT 730, other GPU work on Intel** | yes: each program uses its own GPU | **no** | **yes**, where the other program has an adapter option. GPU Fusion verified Chromium's `--use-adapter-luid` for Brave and Edge [Earlier]. | **yes** | Measured in E1: a 3D program on the GT 730 costs a GPU-bound game up to 77 % more time; on Intel, 0 %. Decode costs nothing on either GPU. | GPU Fusion's monitoring only. If the moved program is visible, Windows copies its window to the GT 730 for display [Hypothesis, E3]. |
| 3 | **Independent game tasks on the Intel GPU** (compute, simulation, procedural generation) | D3D11 pixel shaders only, through system memory | **yes**: a mod, plugin or engine change | through the SDK: the scheduler, D3D11 devices, verification and fallback exist (1.12) | **only for latency-tolerant work.** Per-frame work does not fit the copy latency. | Up to about 1.14–1.2× on GPU-parallel independent work (measured on GPU Fusion's workload). A game gains only if that work was on its critical path. | Copies are measured; the scheduler costs ≤ 0.02 ms per run [Earlier] |
| 4 | **Engine plugins** (Unity native plugin, Godot GDExtension, Java mods through FFM/JNI) | as in 3 | yes, but through the engine's documented extension point | GPU Fusion would ship the plugin and SDK | **yes, for games whose developers or modders adopt it** | as in 3 | as in 3 |
| 5 | **Per-game integration** (Minecraft mod, Roblox) | see section 3 | Minecraft: a mod; Roblox: impossible | no | Minecraft: experimental; Roblox: no | see section 3 | |
| 6 | **Existing GPU selection** | | no | yes; GPU Fusion has done this since 1.1 | yes, but it puts a *whole program* on one GPU. Here that matters only for approach 2. | none for the game itself (the GT 730 is already the faster GPU) | none |
| 7 | **API-level tricks** (OpenGL, D3D11, Vulkan) | OpenGL: Windows has no GPU choice for a window (`WGL_NV_gpu_affinity` is Quadro-only, [Khronos forum](https://community.khronos.org/t/multiple-gpu-and-the-wgl-nv-gpu-affinity-extension/59415)) [Source]. Vulkan: absent on both. D3D11: approach 1b. | | | **no** | | |

**Interception-based "both GPUs for games" did exist once:** Lucid Virtu MVP on Sandy Bridge, with
HyperFormance and Virtual V-Sync. It worked by driver-level interception between the game and the
GPUs, and it mainly cut redundant frames rather than sharing rendering
([AnandTech](https://at-web1.www.anandtech.com/show/5728/intel-z77-panther-point-chipset-and-motherboard-preview-asrock-asus-gigabyte-msi-ecs-and-biostar/3),
[PCPer review](https://pcper.com/2012/08/lucid-logix-virtumvp-first-look-integrated-and-discrete-join-forces/2/))
[Source]. Interception is exactly what GPU Fusion rules out, and the product is discontinued.

## 3. Specific games on this PC

### Minecraft Java Edition (installed: 26.3, vanilla, no mods folder)

- **What it does today** [Log]:
  - It first tries Vulkan: `NativeLibrariesBootstrap.tryLoadingVulkan` fails with "Failed to locate
    library: vulkan-1.dll".
  - It then uses "graphics backend OpenGL, drivers 3.3.0 NVIDIA 391.35, device GeForce GT 730".
  - Rendering is therefore on the GT 730, through OpenGL.
- **Mojang is replacing OpenGL with Vulkan** and will remove OpenGL "only after Vulkan performance
  and stability are considered acceptable". It warns that Vulkan "may not be supported on GPUs that
  are 10 years old or more"
  ([Mojang](https://minecraft.net/article/another-step-towards-vibrant-visuals-for-java-edition),
  [Tom's Hardware](https://www.tomshardware.com/video-games/after-17-years-minecraft-java-edition-starts-replacing-opengl-with-a-vulkan-multi-threaded-renderer-update-will-prove-a-boon-for-performance-and-modding))
  [Source]. Neither GPU here has Vulkan. **Once OpenGL is removed, future Minecraft Java versions will
  not run on this PC at all.** That is a timing risk for any Minecraft work.
- **Moving Minecraft to the Intel GPU** is not possible and not useful:
  - OpenGL on Windows offers no adapter choice for a window [Source].
  - The HD 2000 has OpenGL 3.1, below the 3.3 context Minecraft uses [Log, Source].
  - The HD 2000 is about 5× slower anyway [Earlier].
- **A legitimate integration path exists:** Fabric and NeoForge mods are documented, user-installed
  extensions. Since Java 22, a mod can call a native DLL through the Foreign Function & Memory API
  without JNI glue ([JEP 454](https://openjdk.org/jeps/454)) [Source]. A mod could therefore use the
  GPU Fusion SDK to run work on the HD 2000.
- **What such a mod could offload is the open question [Hypothesis, E4].**
  - **Rendering:** not possible (approach 1).
  - **Chunk meshing and lighting:** CPU work today, branchy, built from Java objects. A poor fit for
    pixel shaders without compute support.
  - **World-generation noise:** uses double precision. The HD 2000 has no doubles at FL 10_1, so the
    results would not be bit-identical and worlds would differ: unacceptable.
  - **A mod's own independent work** (for example generated maps, previews, image processing) is the
    realistic case. It helps only when that work is on the critical path.
  - The i5-2400 is often the bottleneck. The HD 2000 shares its memory bandwidth, and on GPU Fusion's
    integer workload it is 2.7× faster than all four CPU cores [Earlier].
- **Verdict:** **Experimental at best**, and not the first target. Minecraft *is* a good first target
  for approach 2 and for measuring frame times. Both need no mod and work with OpenGL.

### Roblox (installed: Microsoft Store / Xbox GDK package `ROBLOXCorporation.RobloxGDK` 2.744)

- **Graphics:**
  - Roblox's documented graphics modes include Direct3D 11, Vulkan, OpenGL and Metal
    ([GraphicsMode](https://create.roblox.com/docs/reference/engine/enums/GraphicsMode.md)) [Source].
  - Roblox staff say Vulkan "is currently not officially supported … on Windows"
    ([DevForum](https://devforum.roblox.com/t/roblox-crashing-when-minimizing-with-vulkan/3548052))
    [Source].
  - GPU Fusion saw it at about 57 % 3D on the GT 730 and 0 % on Intel [Earlier].
  - The installed package writes only crash-handler logs, so its API is not confirmed here [Test].
- **Extension mechanisms:**
  - Scripts and Studio plugins are Luau and run inside Roblox
    ([Luau](https://create.roblox.com/docs/luau), [plugins](https://create.roblox.com/docs/studio/plugins.md)).
  - No documented way to load native code into the player was found [Source].
  - The client is protected by Hyperion anti-tamper, which crashes the client when "badware" interacts
    with it ([summary](https://breezewiki.discard.no/roblox/wiki/Hyperion)) [Source].
  - Fast-flag files are not an adapter-selection or offload interface. They are contested, and
    Microsoft Store installs differ
    ([DevForum](https://devforum.roblox.com/t/is-there-a-way-to-use-fflags-on-microsoft-store-roblox/2874643))
    [Source].
- **Verdict:** **Unsupported** for any split or offload inside Roblox; that needs Roblox itself to
  integrate. Approach 2 applies unchanged: keep the GT 730 free for Roblox and move other GPU work to
  Intel.

### Other games

- **Minecraft for Windows (Bedrock)** is installed. Its add-ons are sandboxed scripts with no native
  code. **Unsupported** for offload; approach 2 applies.
- **Any other existing game:** approach 2 applies. No commercial game that runs on this hardware is
  known to offload work to a second, dissimilar GPU without D3D12 on both.

## 4. Experiment E1: does other GPU work slow a game, and does Intel take it away? [Test]

`scripts/experiments/game_contention.py`, 4 rounds (3 + 1 confirmation), conditions alternating.

- **Stand-in game:** GPUFusionSplitDemo on the default adapter (GT 730), hash image at standard size,
  median of 20 frames. It keeps the GT 730 near 95 % 3D, like a GPU-bound game, and every output was
  correct.
- **Background:**
  - a continuously rendering 3D program (GPUFusionProbe);
  - or continuous H.264 decoding (GPU Fusion's dispatcher).

| Background load | Stand-in game, median ms per image | Change |
|---|---|---|
| none | 62.94 (62.67–63.06) | — |
| 3D program **on the GT 730** | 111.42 (110.36–112.91) | **+77 % time** |
| the same 3D program **on the Intel GPU** | 62.33 (62.10–62.51) | −1 % (none) |
| H.264 decoding on the GT 730 | 62.74 (62.35–63.01) | none |
| H.264 decoding on the Intel GPU | 62.91 (62.45–63.13) | none |

What it shows:

- 3D work by another program **time-slices the game's GPU**, and putting that program on the Intel GPU
  removes the cost entirely.
- Video decoding runs on a separate engine and does not compete with the game's 3D work, whichever
  GPU does it. *Moving video playback to Intel therefore gains nothing for the game in this test.*
- The background program here rendered as fast as it could, which is a worst case. Real background
  programs (a browser, Discord, a launcher) use much less, so **the realistic gain equals the
  GT 730 3D time those programs take while the game runs.** GPU Fusion already measures that per
  process.

An earlier variant used the probe as the "game". It showed no effect at all, because the probe alone
is limited by draw latency, not by the GPU. That variant is not used as evidence.

## 5. Compatibility categories (proposed)

| Category | Definition | Today |
|---|---|---|
| **Supported** | A reproducible integration does useful work on both GPUs, with output and gain measured | No game. Only GPU Fusion's own Workload Lab and `GPUFusionSplitDemo` (SDK). |
| **Experimental** | A legitimate integration exists; its benefit or stability is not yet established | **Game on the GT 730 with Chromium browsers (Brave, Edge) moved to Intel**: the mechanism is verified, the game benefit is not yet measured with a real game (E2). |
| **Unsupported** | No interface on this hardware lets the game use both GPUs | Splitting any game's rendering. Roblox (inside the game). Minecraft Java and Bedrock (inside the game, without a mod). |

**Good first targets for real integration (approach 4)** prefer documented native extension points:

1. **Godot 4** (open source; GDExtension loads native libraries). An engine-level sample game can be
   built and measured end to end.
2. **Minecraft Java through a Fabric mod** (FFM native call). Experimental, given the Vulkan
   migration risk and the lack of a natural offload candidate.
3. **Luanti (Minetest)**: open-source C++ and OpenGL, so an engine integration is legitimate; its Lua
   mods cannot load native code.

## 6. The answers

1. **Can an existing game on this PC realistically benefit from both GPUs through legitimate
   application-level integration?**
   - **Not by sharing its rendering:** that needs D3D12 or Vulkan on both GPUs, and the HD 2000 has
     neither.
   - **Yes, indirectly,** by keeping the GT 730 for the game and putting other programs' 3D work on
     the Intel GPU. Measured with a GPU-bound stand-in, a 3D background program on the GT 730 cost
     77 % more time, and on the Intel GPU nothing.
   - How much a real game gains depends on the background 3D load at the time; this has not yet been
     measured with a real game.
2. **Is Minecraft Java Edition a practical first target?**
   - For in-game offload, no: no good offload candidate, double-precision world generation, and
     Mojang's move to Vulkan will eventually drop this PC.
   - For approach 2 and for frame-time measurement, yes: no mod is needed, and its log confirms it
     renders on the GT 730.
3. **Is Roblox supportable without Roblox integrating with GPU Fusion?** No, for anything inside the
   game. There is no native extension point and there is anti-tamper protection. Only approach 2
   helps Roblox.
4. **What is the best achievable game-related improvement for this hardware?**
   - **The game alone on the GT 730, every other GPU consumer on the Intel GPU,** with frame times
     measured before and after.
   - For engines that integrate the SDK: up to about 1.14× on independent, latency-tolerant GPU work.
5. **What should be implemented next?** The plan below: first measure game frame times, then offload
   companion programs measurably, then build one engine plugin.

## 7. Implementation plan (not started)

**Step 1. Game frame-time measurement**

- Record the present events of a chosen running game through ETW, as Intel's open-source PresentMon
  does.
- This is passive: nothing is loaded into the game, and it covers D3D, OpenGL and Vulkan games
  ([Tom's Hardware](https://www.tomshardware.com/reviews/presentmon-performance-directx-opengl-vulkan,4740.html)).
- A real-time ETW session needs administrator rights or membership of "Performance Log Users"
  ([PresentMon README](https://raw.githubusercontent.com/GameTechDev/PresentMon/main/README.md)).
  GPU Fusion will ask each time (an elevated helper started only on request) and never change group
  membership by itself.
- Output: frame-time median, percentiles and stutter, saved as a Game Lab session with the existing
  significance rule.
- *Experiment E2:* does capture work with Roblox (Hyperion) and Minecraft (OpenGL on the GT 730)?
  [Hypothesis]

**Step 2. Game companion offload**

- While a profiled game works on the GT 730, list the other processes with GT 730 3D time and their
  share; the existing per-process monitoring supplies this.
- Mark those with a verified legitimate adapter option: Chromium browsers yes; Electron/Discord not
  verified.
- Offer to start them on the Intel GPU through their profile.
- Never close or restart a running program without the user's explicit choice (the existing graceful
  close asks first).
- *Experiment E3:* the Windows display copy cost for a visible window rendered on Intel, measured with
  step 1's frame times.

**Step 3. Before/after verdict**

- Game frame times with the companions on the GT 730, then on Intel, using the same significance
  rule as everywhere else.
- A game moves to "Experimental (measured)" only with a measured result, whatever the direction.

**Step 4. One engine plugin (approach 4)**

- Build the SDK as a C-ABI DLL: devices, the 1.12 scheduler, verification and fallback.
- Wrap it in a Godot GDExtension sample with an independent, latency-tolerant workload, and measure
  frame times with and without the Intel helper.
- A Minecraft Fabric mod over the same DLL (FFM) only if E4 finds a real offload candidate.

**Not planned, because it is impossible or not allowed here:**

- splitting rendering;
- Vulkan or D3D12 multi-adapter;
- injecting into Roblox or any game;
- installing another Intel driver for Quick Sync encoding (driver changes are not authorised).
