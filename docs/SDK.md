# GPU Fusion SDK (optional)

GPU Fusion is a generic GPU manager: it works with any executable, and no program needs to know about
it. The SDK is for one specific case: a program that wants to **follow the GPU choice made in GPU
Fusion**, even where Windows cannot enforce that choice, or that wants to **divide its own work between
GPUs** when the user picks "Both GPUs".

- Header-only: [`sdk/include/gpufusion/gpufusion.h`](../sdk/include/gpufusion/gpufusion.h)
  (C++17, Windows, DXGI, MIT License). Nothing to link and no DLL to ship.
- A program that is not launched by GPU Fusion gets the default adapter. Nothing breaks.

## The contract

When GPU Fusion starts a program from its profile (GPU Manager → *Launch* / *Verify on this PC*,
`--launch-profile`, `--verify-profile`), or runs it as a job in the dispatcher, it sets these variables:

| Variable | Value |
|---|---|
| `GPUFUSION_MODE` | `automatic`, `integrated`, `discrete` or `both` |
| `GPUFUSION_ADAPTERS` | comma-separated adapter LUIDs `HHHHHHHH_LLLLLLLL`, in order of use (Both: discrete first, then integrated). Empty for Automatic. |
| `GPUFUSION_PROFILE` | the profile id (not set for dispatcher jobs) |

LUIDs are only valid until the next restart, so they are passed fresh at every launch. A profile can
switch this off with "Tell the program which GPU to use…".

## Using it

```cpp
#include <gpufusion/gpufusion.h>
#include <d3d11.h>

Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
CreateDXGIFactory1(IID_PPV_ARGS(&factory));
auto adapters = gpufusion::adapters(factory.Get());   // requested adapters that exist, else the default

Microsoft::WRL::ComPtr<ID3D11Device> device;
D3D11CreateDevice(adapters[0].Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                  D3D11_SDK_VERSION, &device, nullptr, nullptr);

if (gpufusion::mode() == gpufusion::Mode::Both && adapters.size() > 1) {
    // One device per adapter; the program divides *independent* work itself, e.g. rows of an image:
    auto parts = gpufusion::split(height, { 5.0, 1.0 });  // weights, ideally measured speeds
}
```

| Function | Purpose |
|---|---|
| `mode()` | the mode GPU Fusion launched the program with, or `NotLaunchedByGpuFusion` |
| `requestedLuids()` | the LUIDs from `GPUFUSION_ADAPTERS` |
| `adapters(factory)` | the requested adapters present on this PC, in order. If none match, the default adapter |
| `split(total, weights)` | consecutive ranges proportional to the weights; every item is assigned exactly once |

Any API that can pick an adapter by LUID works the same way: Direct3D 11/12 (`IDXGIAdapter`),
Vulkan (`VkPhysicalDeviceIDProperties::deviceLUID`), CUDA (`cudaDeviceProp::luid`), OpenCL
(`cl_khr_device_uuid` LUID) and DirectML (through a D3D12 device).

## Verified on the reference PC

`GPUFusionProbe.exe` (in this repository, `tools/probe`) uses the SDK. With GPU Fusion profiles
(`tests/tst_profiles_live.cpp`, `tests/tst_gpumanager_ui.cpp`):

| Profile mode | Windows value written | Observed |
|---|---|---|
| Integrated | none (Windows cannot place programs on the Intel GPU here) | the probe's GPU work ran **only on Intel HD Graphics** |
| Discrete | `GpuPreference=2;` | rendered on the GT 730 only |
| Both | none | the GT 730 rendered (3D) and the Intel GPU did work (its WDDM 1.2 driver does not name engine types) |
| Automatic | none | rendered on the GT 730 |

In **Both** mode, the observation shows that both GPUs did work for the program. It does not show
that the program got faster. Whether dividing the work pays off has to be measured, as the
Workload Lab does for GPU Fusion's own workloads.

## What the SDK cannot do

- It cannot make one rendering workload (one swap chain, one frame) run on two GPUs. That needs the
  program's own explicit multi-adapter design. On this PC it is impossible anyway: the Intel GPU
  has no Direct3D 12, and there is no cross-vendor resource sharing.
- Resources cannot be shared between GPUs of different vendors without a copy through system memory.
- It cannot affect programs that do not include it. Those programs follow Windows' per-app
  preference (where that works) or their own GPU setting.
