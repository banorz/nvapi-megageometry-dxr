# amd-megageometry

**An experimental attempt at a wrapper for NVIDIA RTX Mega Geometry on AMD GPUs.**

Games and demos built for NVIDIA RTX Mega Geometry use NVIDIA's proprietary NvAPI cluster
extensions: cluster acceleration structures (CLAS), cluster templates, cluster BLAS, partitioned
TLAS, and the `nvHLSLExtns` shader intrinsics. They do not run these paths on other GPUs.
`amd-megageometry` replaces `nvapi64.dll` and translates those calls, at runtime, to the
vendor-neutral **DirectX Raytracing 1.722 preview** (Clustered Geometry and Partitioned TLAS,
Shader Model 6.10). The target is AMD Radeon RX 9000 (RDNA 4).

> **Status: experimental, paused.** It works on NVIDIA's public RTX Mega Geometry sample
> (RTXMG 2.2.0). It does **not** work in Alan Wake 2 yet: the GPU faults while building
> cluster BLAS from the game's real data, most likely in the preview AMD driver. Work resumes
> when AMD ships a non-preview driver with DXR 1.722. Details are in [Status](#status).

This project is not affiliated with, endorsed or sponsored by NVIDIA, AMD, Microsoft or Remedy
Entertainment. NVIDIA, RTX, AMD, Radeon and DirectX are trademarks of their respective owners.

---

## Contents

- [Status](#status)
- [How it works](#how-it-works)
- [Requirements](#requirements)
- [Dependencies](#dependencies)
- [Build](#build)
- [Testing guide](#testing-guide)
- [Configuration (environment variables)](#configuration-environment-variables)
- [Reporting a problem](#reporting-a-problem)
- [Legal notes](#legal-notes)

---

## Status

Tested on a Radeon RX 9070 XT with the *AMD Software: Agility SDK Developer Preview Edition
26.20.12.01* driver, Agility SDK `1.722.0-preview` and DXC `1.10.2605.37`.

| Target | Result |
|---|---|
| Test suite (`tests\run_tests.ps1`) | Passes: NvAPI proxy, DXIL patcher, argument conversion, state restore, synthetic cluster builds, capture/replay. |
| NVIDIA RTXMG 2.2.0 sample, `--cluster-api nvapi` | Runs through the wrapper: cluster LoD scenes (for example *ABeautifulGame*) and template scenes (*Barbarian*). Tested with OptiScaler/AMD-NR for DLSS Ray Reconstruction, including runs of more than 14,000 cluster operations. |
| Alan Wake 2 | **Fails.** The game starts and the menu works. After pressing *Continue*, the GPU faults inside `BUILD_BLAS_FROM_CLAS` and the device is removed, often with a full system hang. |

### What is known about the Alan Wake 2 crash

The investigation is documented step by step in [.agent/architecture.md](.agent/architecture.md)
("Diagnostica cluster", Italian). In short:

- Templates (17,397) and instantiations (~33,500) complete. The fault happens while the BLAS
  are built from the instantiated CLAS.
- All CLAS references read by the BLAS are valid CLAS. There are no null or duplicate
  destinations, the memory layout matches what the driver reports, and indices and formats are
  consistent.
- A synthetic reproduction with every known trait of the game passes: index formats, absolute
  16-bit indices, address spans, hints, `FAST_TRACE`, GPU counts, split execution, mixed BLAS
  sizes, geometry indices, duplicate cluster IDs, dirty scratch, and the game's BLAS+CLAS
  memory layout.
- If the game's CLAS are replaced by a single wrapper-built CLAS, all 501 BLAS build.
- Radeon GPU Detective reports the fault in a driver-internal dispatch, at an address that
  belongs to no resource, with no shader in flight.

So the failure depends on the real cluster data and happens inside the driver. Since DXR 1.722
is still a preview on both the Microsoft and the AMD side, the project is paused until a final
driver is available. For that run, the wrapper can capture the real cluster data
(`AMD_MG_CAPTURE_REAL`), and `replay_capture.exe` can rebuild it outside the game (see
[Alan Wake 2](#4-alan-wake-2-experimental-crashes)).

---

## How it works

One binary, `nvapi64.dll`, which can also be loaded as an OptiScaler ASI plugin. It has four parts:

1. **NvAPI proxy** ([src/nvapi_proxy.cpp](src/nvapi_proxy.cpp)). Exports `nvapi_QueryInterface` and
   implements the Mega Geometry entry points: raytracing caps, cluster operation requirements,
   multi-indirect cluster operations, partitioned TLAS and the shader extension slot. Every other
   NvAPI call is forwarded to the next `nvapi64.dll` in the chain, for example OptiScaler's
   internal fakenvapi.
2. **GPU argument converter** ([shaders/arg_converter.hlsl](shaders/arg_converter.hlsl),
   [src/gpu_arg_converter.cpp](src/gpu_arg_converter.cpp)). Mega Geometry is GPU-driven: the
   per-cluster arguments are written by the game's compute shaders. Before each
   `ExecuteIndirectRTASOperations`, the wrapper records a compute dispatch on the game's command
   list that rewrites the NvAPI argument layouts into DXR 1.722 layouts (for example, CLAS from
   triangles goes from 64 to 80 bytes and templates from 72 to 96 bytes) and converts the result
   size arrays. The layout tables are in [.agent/translation.md](.agent/translation.md).
3. **DXIL patcher** ([src/dxil_patcher.cpp](src/dxil_patcher.cpp)). When the game creates a
   raytracing state object, the wrapper rewrites shaders that use the NvAPI HLSL extensions
   (`NvRtGetClusterID`, `NvRtTriangleObjectPositions` and their RayQuery and HitObject variants)
   into Shader Model 6.10 intrinsics. It then revalidates and re-signs the container with
   `dxcompiler.dll` and `dxil.dll`.
4. **D3D12 hooks** ([src/d3d12_hook.cpp](src/d3d12_hook.cpp),
   [src/cmdlist_state.cpp](src/cmdlist_state.cpp)). These enable the Agility SDK 1.722 preview
   and the experimental features, add the clustered-geometry pipeline flag, and redirect the
   `LoadLibraryExW("nvapi64.dll", LOAD_LIBRARY_SEARCH_SYSTEM32)` made by NVIDIA's static
   `nvapi64.lib`. They also save and restore the game's compute state around the wrapper's own
   dispatches, because NvAPI on NVIDIA does not touch that state.

Developer documentation, in Italian, is in [.agent/](.agent/agent.md): architecture, data flow,
deployment gotchas, the full environment variable list and the troubleshooting notes.

---

## Requirements

- **Windows 11** with **Developer Mode** enabled (Settings > System > For developers). Without
  it, D3D12 refuses the Agility SDK preview and the experimental features. Check it with:
  ```powershell
  Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock" -Name AllowDevelopmentWithoutDevLicense
  ```
- **AMD Radeon RX 9000 (RDNA 4)** with a driver that reports `ClustersAndPTLASSupported = TRUE`
  for DXR 1.722. This was tested with
  [AMD Software: Agility SDK Developer Preview Edition 26.20.12.01](https://www.amd.com/en/resources/support-articles/release-notes/RN-RAD-MS-AGILITY-SDK-26-20-12-01.html).
  Regular Adrenalin drivers available at the time reported `FALSE`. Other GPUs whose driver
  supports DXR 1.722 clustered geometry might work, but this has not been tested.
- **Visual Studio 2022 or newer** with *Desktop development with C++* (MSVC, C++20). The build
  script finds it with `vswhere`; set `VSDEVCMD` to the path of `VsDevCmd.bat` to choose a
  specific install.

## Dependencies

Third-party SDKs are not part of the repository. Put them in `sdk\` with this layout, which
`build.ps1` expects:

| Folder | What | Where to get it |
|---|---|---|
| `sdk\agility\extracted\` | Microsoft Agility SDK **1.722.0-preview** (`build\native\include`, `build\native\bin\x64`) | NuGet package [Microsoft.Direct3D.D3D12 1.722.0-preview](https://www.nuget.org/packages/Microsoft.Direct3D.D3D12/1.722.0-preview). A `.nupkg` is a zip: extract it here. |
| `sdk\dxc\extracted\` | DirectX Shader Compiler **1.10 preview** (tested: `1.10.2605.37`), with `bin\x64` and `inc` | [microsoft/DirectXShaderCompiler releases](https://github.com/microsoft/DirectXShaderCompiler/releases): the Windows zip of the 1.10 preview |
| `sdk\nvapi\repo\` | NVAPI headers (`nvapi.h`, `nvHLSLExtns.h`, ...), MIT license | `git clone https://github.com/NVIDIA/nvapi sdk\nvapi\repo` (tested at commit `70d337d`) |
| RTXMG sample (optional, for testing) | NVIDIA RTX Mega Geometry sample **2.2.0**, binary release | [NVIDIA-RTX/RTXMG releases](https://github.com/NVIDIA-RTX/RTXMG/releases). It is under NVIDIA's own license; extract it anywhere, for example `sdk\rtxmg_bin\`. |

```powershell
# Example: NVAPI headers
git clone https://github.com/NVIDIA/nvapi sdk\nvapi\repo
# Example: Agility SDK (download the .nupkg from nuget.org first)
Expand-Archive .\microsoft.direct3d.d3d12.1.722.0-preview.nupkg -DestinationPath sdk\agility\extracted
# DXC: extract the release zip so that sdk\dxc\extracted\bin\x64\dxc.exe exists
```

## Build

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

Outputs in `build\`:

- `nvapi64.dll`: the wrapper.
- `dxcompiler.dll`, `dxil.dll`: DXC 1.10, needed next to the wrapper by the DXIL patcher.
- `D3D12\`: Agility SDK 1.722 (`D3D12Core.dll`, `D3D12StateObjectCompiler.dll`, `d3d12SDKLayers.dll`).
- Test tools: `test_wrapper.exe`, `repro_templates.exe`, `replay_capture.exe`, `repro_rtpso.exe`, `dxil_asm.exe`.

---

## Testing guide

> **Warning:** everything below runs real GPU work on a preview driver. A driver fault can cause
> a TDR (the screen goes black for a few seconds) or freeze the system. After a GPU crash,
> D3D12 often does not work again until you reboot. Save your work first.

### 1. Test suite

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\run_tests.ps1
```

It runs these steps in order and stops at the first failure:

| Step | What it checks |
|---|---|
| `test_wrapper.exe` | Device and DXR 1.722 caps, NvAPI caps through the proxy, DXIL patching of a test shader (and of the RTXMG library if `build\native_nv.dxil` exists), argument conversion layouts, compute state restore, DRED dump on device removal. Expected: `Summary: N PASSED, 0 FAILED`. |
| `repro_templates.exe` (4 runs) | Builds cluster templates, instances and BLAS through `build\nvapi64.dll` with synthetic data: an RTXMG-like baseline, all Alan Wake 2 traits (`--aw2`), BLAS of 10 CLAS, and the game's BLAS+CLAS memory layout (`--pack`). Each run must print `RESULT: OK`. Guard zones check that the driver writes no more than the sizes it reports. |
| capture + `replay_capture.exe` | Captures the cluster data of a synthetic run (`AMD_MG_CAPTURE_REAL=4`), then rebuilds it from the file. Both must print `RESULT: OK`. |

Use `-SkipGpuRepros` to run only `test_wrapper.exe`. All `AMD_MG_*` variables from your
environment are cleared for the run. The wrapper log of the last step is
`build\amd_megageometry.log`. `repro_templates.exe` can also be run by hand: its options are
listed at the top of [tests/repro_templates.cpp](tests/repro_templates.cpp).

### 2. Shader compiler repro (optional)

If the driver crashes while compiling raytracing pipelines, extract the patched library
(`AMD_MG_DUMP_DXIL=1`) and replay it outside the application:

```powershell
cd build
.\repro_rtpso.exe patched_cluster_shader.dxil                       # whole library
.\repro_rtpso.exe patched_cluster_shader.dxil --only=RayGen,ClusterLodClosestHit   # bisect by export
..\sdk\dxc\extracted\bin\x64\dxc.exe -dumpbin patched_cluster_shader.dxil -Fc x.ll   # edit the IR, then:
.\dxil_asm.exe x.ll x.dxil; .\repro_rtpso.exe x.dxil
```

### 3. NVIDIA RTXMG sample

The sample uses Streamline, and the tested setup uses OptiScaler with the AMD-NR build for DLSS
Ray Reconstruction. Deploy to the sample's `bin\` folder:

1. Install OptiScaler/AMD-NR into `bin\` as `dxgi.dll` (follow that project's instructions).
2. Copy `build\nvapi64.dll` into `bin\`, and copy it again as `bin\OptiScaler\plugins\amd_megageometry.asi`.
   Set `LoadAsiPlugins = true` in the `[Plugins]` section of `OptiScaler.ini`.
3. Copy `build\dxcompiler.dll`, `build\dxil.dll` and the `build\D3D12\` folder into `bin\`.
4. Run a cluster LoD scene through the wrapper, then the native DXR path of the sample for
   comparison:
   ```powershell
   cd bin
   .\rtxmg_demo.exe -mf cluster_lod/ABeautifulGame/glTF/ABeautifulGame.gltf --cluster-api nvapi
   .\rtxmg_demo.exe -mf cluster_lod/ABeautifulGame/glTF/ABeautifulGame.gltf --cluster-api dxr
   ```
   Useful extra options are `--dlssMode off` and
   `--dolly-frames 240 -nf 3000 --dump-stats stats.json` (repeatable benchmark path).
5. Check `bin\OptiScaler\plugins\amd_megageometry.log`. It should contain
   `GetRaytracingCaps(CLUSTER_OPERATIONS)`, cluster operations being converted, and
   `[DXIL] Validator succeeded` for the patched pipelines.

Known issue that is not caused by the wrapper: if OptiScaler reports DLSS-RR as unavailable
(`[FSR-RR] RrBackend=off`) while the sample keeps DLSS-RR on, the sample gets an invalid render
size. After about 2,000 frames it then runs out of descriptor heap space. Use
`RrBackend=auto`, or turn off *Enable DLSS-RR* in the sample's GUI.

### 4. Alan Wake 2 (experimental, crashes)

This is how the crash was investigated. Only try it if you can live with a hard GPU crash.

1. Install OptiScaler/AMD-NR into the game folder as `dxgi.dll` and enable the DXGI spoofing
   option (the game must see an NVIDIA GPU to use Mega Geometry).
2. Copy `build\nvapi64.dll` into the game folder and as `OptiScaler\plugins\amd_megageometry.asi`
   (with `LoadAsiPlugins = true`). Also copy `build\dxcompiler.dll` and `build\dxil.dll`.
3. The game ships `D3D12Core.dll` both in its root and in `D3D12\`. Back up the originals, then
   copy the files from `build\D3D12\` into both places.
4. Next to the ASI, create `amd_megageometry.ini`. The game is started by a launcher, so
   environment variables are not passed to it; the wrapper reads `AMD_MG_*=value` lines from
   this file instead:
   ```ini
   # Alan Wake 2 uses 16-bit indices that are absolute within the mesh
   AMD_MG_INDEX_FORMAT=16
   # Capture the real cluster data of the first 16 BLAS
   AMD_MG_CAPTURE_REAL=16
   ```
5. Start the game, load a save, press *Continue*. If it crashes, the wrapper writes the DRED
   report into `amd_megageometry.log` and the capture into `amd_megageometry_capture.bin`, next
   to the ASI.
6. Replay the capture outside the game:
   ```powershell
   copy "<game>\OptiScaler\plugins\amd_megageometry_capture.bin" build\
   cd build
   .\replay_capture.exe amd_megageometry_capture.bin              # all captured BLAS
   .\replay_capture.exe amd_megageometry_capture.bin --only=4     # a single BLAS
   ```
   If the replay also faults, the problem can be bisected offline down to the guilty clusters.
   If the replay passes, the problem depends on the game's state rather than on the data.

The capture file contains the game's geometry. Do not publish it. Share it only privately, for
example with AMD for a driver bug report.

To compare the game with and without Mega Geometry, set `AMD_MG_DISABLE=1`. The game then sees
cluster operations as unsupported, and everything else stays active.

---

## Configuration (environment variables)

You can set these as process environment variables, or as `NAME=value` lines in
`amd_megageometry.ini` next to the DLL or ASI. A variable that is already set in the
environment takes precedence over the ini. The full list, with the diagnostic and A/B switches,
is in [.agent/agent.md](.agent/agent.md).

| Variable | Default | Effect |
|---|---|---|
| `AMD_MG_LOG` | `1` | Writes `amd_megageometry.log` next to the DLL. |
| `AMD_MG_DISABLE` | `0` | `1`: reports Mega Geometry as unsupported, for A/B comparisons. |
| `AMD_MG_INDEX_FORMAT` | `8` | Index format declared to DXR for clusters built from triangles: `8`, `16` or `32`. Alan Wake 2 needs `16`. |
| `AMD_MG_DRED` | `1` | DRED breadcrumbs and page-fault reports in the log when the device is removed. Set `0` for benchmarks. |
| `AMD_MG_DUMP_ARGS` | `1` | Decoded dump of the first cluster operations of each type, and the layout analysis. |
| `AMD_MG_DUMP_DXIL` | `0` | `1`: saves the last patched shader library as `patched_cluster_shader.dxil`. |
| `AMD_MG_CAPTURE_REAL` | `0` | `N`: captures the real cluster data of the first N BLAS into `amd_megageometry_capture.bin`. |
| `AMD_MG_SPLIT_BLAS` | `1` | Builds BLAS from CLAS in chunks of N, so that the log can name the BLAS in flight. `0` runs one operation as the game requested. |
| `AMD_MG_FORCE_CAPS` | `0` | `1`: reports Mega Geometry caps even without `ID3D12DeviceRaytracing2`. |

---

## Reporting a problem

Attach `amd_megageometry.log`, together with the OptiScaler log if you use it, and give your
GPU, driver version and Windows build. A Radeon GPU Detective report helps with GPU crashes.
Never attach game files, capture files or NVIDIA binaries.

## Legal notes

- This repository contains only original code. It uses the NVAPI headers from
  [NVIDIA/nvapi](https://github.com/NVIDIA/nvapi), which are MIT licensed, at build time. They
  are not included in this repository.
- No NVIDIA SDK, sample, DLSS binary or model is included or redistributed. The RTXMG sample is
  only used as a test target and must be downloaded from NVIDIA under NVIDIA's license.
- The Agility SDK and DXC are downloaded from Microsoft under Microsoft's licenses.
- This is an independent interoperability project, provided as is, without warranty.

## License

[MIT](LICENSE)
