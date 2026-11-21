# OpenScarlett

[![Windows CI](https://github.com/californiaAlmonds/OpenScarlett/actions/workflows/ci.yml/badge.svg)](https://github.com/californiaAlmonds/OpenScarlett/actions/workflows/ci.yml)
[![Latest Hardware Test Build](https://img.shields.io/github/v/release/californiaAlmonds/OpenScarlett?label=download%2060s%20test%20bundle&color=2ea043)](https://github.com/californiaAlmonds/OpenScarlett/releases/latest)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

**OpenScarlett** is an open-source, MIT-licensed **Xbox Series X|S (`Scarlett`) translation layer for Windows 10 & Windows 11 (x64)**.

Because Xbox Series X|S runs on an **AMD Zen 2 x86-64 CPU** and a **Windows-derived Game OS**, games do not need slow CPU emulation. Instead, OpenScarlett runs the game's native x86-64 instructions directly on your PC while translating the Xbox-specific graphics (`d3d12_xs`), precompiled RDNA2 GPU shaders, memory aliasing (`kernelx`), audio/media codecs, and GDK runtime APIs to native Windows DirectX 12 & Vulkan.

> [!IMPORTANT]
> ### 🧪 Help Us Test OpenScarlett on Your GPU — 60 Seconds, No Visual Studio Needed!
>
> OpenScarlett has been live-tested on **6 real PCs** so far (**NVIDIA GTX 1650 Ti, RTX 2060 SUPER & RTX 3080 Ti**, plus **AMD Radeon RX 7600, RX 7900 XTX & RX 9070 XT** across **Win10 22H2, Win11 25H2 & Win11 26H2** — still zero Intel Arc, RX 6000/handheld APU, or RTX 40/50 reports!). Every new GPU and Windows build helps us catch real driver, shader, and memory-aliasing quirks we cannot reproduce locally!
>
> **How to help in 3 clicks:**
> 1. **[⬇️ Download the latest `OpenScarlett-*-win64.zip`](https://github.com/californiaAlmonds/OpenScarlett/releases/latest)** and extract anywhere *(everything — including open-source `dxc.exe` — is bundled; zero coding or SDK required)*.
> 2. **Double-click `run_hardware_test.bat`** — runs all GPU/OS tests live with clear progress and automatically strips usernames & local folder paths.
> 3. **Press `Enter`** when prompted — opens a **pre-filled GitHub Hardware Report** in your browser. Just review and click **Create**! *(Failures on new hardware are just as valuable as passes!)*
>
> | Most Wanted Hardware | Why It Helps OpenScarlett |
> | :--- | :--- |
> | **AMD Radeon RX 6000 (RDNA2) & Handheld APUs** *(ROG Ally, Legion Go, Steam Deck Windows)* | RX 7600, RX 7900 XTX & RX 9070 XT already pass! Still need native **RDNA2** (exact Series X\|S sibling) + UMA handheld APU coverage |
> | **Intel Arc (A/B-Series) & Integrated GPUs** *(Xe / Vega / RDNA iGPU)* | Subgroup/wave-size edge cases (`Wave8`/`Wave16`/`Wave32`) + UMA memory heap handling |
> | **NVIDIA RTX 40 / 50 Series** | RTX 2060 SUPER & RTX 3080 Ti pass (`DXR=12`) — still want Ada/Blackwell SM 6.7–6.8 validation |
> | **Windows 11 (23H2 / 24H2)** | Win10 22H2, Win11 25H2 & Win11 26H2 verified — still want older Win11 servicing builds |

---

> **Legal & Clean-Room Notice:** OpenScarlett is 100% clean-room MIT-licensed C++20 code. It contains **no** proprietary Microsoft/Xbox binaries, encryption keys, firmware, or copyrighted game assets, and **never decrypts retail `.xvc`/`.xvd` packages**. You must supply your own legally obtained, unencrypted GDK binaries or open-source samples.

---

<!-- OPENSCARLETT_STATUS_START -->
## Live Project Status

![OpenScarlett Status](docs/status.svg)

**Overall Clean-Room Readiness:** `100.0%` · **Automated Tests:** `29/29` passing · **Verified Hardware (6 PCs):** `Win10 + GTX 1650 Ti` · `Win11 25H2/26H2 + RTX 2060 SUPER & RTX 3080 Ti (DXR=12)` · `Win11 26H2 + AMD RX 7600, RX 7900 XTX & RX 9070 XT (Wave64/DXR=11)`

| Subsystem | Area | Status | Progress | What Works Right Now | Verified On |
| :--- | :--- | :--- | :---: | :--- | :--- |
| **PE32+ Loader, `MicrosoftGame.Config`, CoW VFS & Topo Init** | `Loader` | 🟢 Live-Binary Verified | `100%` | `MicrosoftGame.Config` XML loader, CoW overlay VFS (`G:/GAME:/T:/S:`), PE forwarders/TLS/`.pdata` SEH, Kahn topological DLL init ordering, 19-DLL routing, `--backend=vulkan`, APC hook + unified telemetry | Win10 & Win11 26H2 (`test_pe_inspect`, `test_apc_loader`, `test_loader_vfs_config`, `test_gdk_extended_loader`, `test_gdk_live`) |
| **Vulkan 1.2/1.4 Dynamic Graphics, `VK_EXT_mesh_shader` & Swapchain** | `Graphics` | 🟢 Live-GPU Verified | `100%` | Zero-SDK `vulkan-1.dll` rasterization (`RGBA8`/`FP16`/`D32F`/4× MSAA resolve), native `VK_EXT_mesh_shader` (`Task+Mesh+PS`), mipmapped 2D/3D volume textures, Win32 swapchain + HDR10 probe, SPIR-V 1.3/1.4 + SW fallback | NVIDIA GTX 1650 Ti (Vk 1.4.329, `VK_EXT_mesh_shader=task+mesh`, `0.0000` MAE vs D3D12), RTX 2060 SUPER/3080 Ti, AMD RX 7600/7900 XTX/9070 XT (`test_vulkan_live`, `test_vulkan_graphics_live`, `test_vulkan_mesh_showcase`) |
| **Codecs, Opus/WASAPI, `X3DAudio`/`XAudio2FX` & GPU FSR 1.0** | `Media & IO` | 🟢 Live-GPU Verified | `100%` | GDeflate v1, BCPack (BC1–BC7), XMA2 IMDCT, `MSOpusDecoder.dll` Opus (`0x704F`), IMA-ADPCM, live WASAPI, `X3DAudio` (panning/Doppler) & `XAudio2FX` (Reverb/VolumeMeter), Bink2 `KB2i`, live GPU FSR 1.0 | 6 PCs (GTX 1650 Ti, RTX 2060 SUPER/3080 Ti, AMD RX 7600/7900 XTX/9070 XT) + WARP (`test_codecs`, `test_audio_opus_wasapi`, `test_gdk_extended_loader`, `test_d3d12_xs_live`) |
| **RDNA2 AAA Shader Lifter, ISA Hardening & Disk Cache** | `Shaders` | 🟢 Live-GPU Verified | `100%` | RDNA2 AAA disasm/IR/lifter (zero-init LDS, exact `f16` FMA vs denorm-flushing MAD, encoding-size 16-bit `VOP3`, `s_bcnt1` b32/b64, quad `ddx`/`ddy`, bounded `wave64`→`wave32` mesh, `SCRATCH`, `BVH_INTERSECT_RAY`), CPU wave32/64 interpreter, disk cache + `d3d12_xs` auto-lift | `spirv-val` + `dxc` SM 6.5 + 6 GPUs (NVIDIA Turing/Ampere + AMD RDNA3/RDNA4 `MeshTier=10`) + WARP `1.0.22` guard (`test_rdna2_core`, `test_rdna2_lifter_live`, `test_d3d12_xs_autolift`) |
| **WinRT Activation & HSTRING (`winrt_xs.dll`)** | `Runtime` | 🟢 Live-Binary Verified | `100%` | 20 exports: `RoInitialize`, activation factories, zero-alloc `HSTRING`, Xbox `AnalyticsInfo`/`ApplicationData` + `CallTelemetry` | Win10 & Win11 26H2 (`test_winrt_xs`, `test_apc_loader`, `test_gdk_live`) |
| **Xbox Unified Memory, `GpuTilePool` & `kernelx.dll`** | `Memory` | 🟢 Live-Binary Verified | `100%` | Win10/11 `VirtualAlloc2`/`MapViewOfFile3` 64 KiB & 4 MiB physical-virtual aliasing, `GpuTilePool`, placed unified-memory texture remap tracking, 41 `kernelx.dll` exports (`xmem.dll` forwarding) | Win10 & Win11 26H2 (`test_kernelx`, `test_hdr10_d3d12`, `test_secondary_shims`, `test_gdk_live`) |
| **`d3d12_xs.dll` Proxy, HDR10 `PresentX` & Full `xg_xs`** | `Graphics` | 🟢 Live-GPU Verified | `100%` | `DeviceX` auto-lift + default `Texture2DArray` SRV slice preservation + fail-closed `E_NOTIMPL`, HDR10 (`R10G10B10A2` ST.2084) & scRGB (`FP16`) `PresentX` with ACES/Reinhard SDR tone-map fallback + 60/120 Hz pacing, full GFX10 `xg_xs` (15 2D + 6 3D + mip tails) | 6 GPUs (GTX 1650 Ti, RTX 2060 SUPER/3080 Ti, AMD RX 7600/7900 XTX/9070 XT `MeshTier=10`) + WARP (`test_d3d12_xs_live`, `test_presentx_windowing`, `test_hdr10_d3d12`, `test_xg_dstorage`) |
| **GDK & Console Shims (19 DLLs + `XDisplay`/`XGameUI`/`QueryApiImpl`)** | `GDK Shims` | 🟢 Live-Binary Verified | `100%` | 19 console/GDK DLLs: `xgameruntime` (12-table `QueryApiImpl`, `XDisplay` HDR, `XLauncher`/`XGameUI`, `XGameInvite`, `XAccessibility`, `XSpeechSynthesizer`), `GameInput`, `XAudio2_9`/`X3DAudio`/`AcpHal`, `dstorage_xs`, `XSAPI`, `pixevt`, `xmem`, `xcrdapi`, `libHttpClient.GDK`, `XCurl`, `Bink2Scarlett`, FidelityFX, `XboxBvhBuilder` | Win10 & Win11 26H2 (`test_gdk_shims`, `test_gdk_extended_loader`, `test_secondary_shims`, `test_xg_dstorage`, `Xbox-GDK-Samples`, 19-DLL `test_external_gdk_bringup`) |
| **DXR Tier 1.1, HDR10 & Cross-API Real-Binary GDK Workloads** | `Graphics` | 🟢 Live-GPU Verified | `100%` | `ID3D12Device5` BLAS/TLAS/SBT + `lib_6_3` `DispatchRays`, `SimplePBR`, `MeshletCull`, `SimpleLighting` (D3D12+Vk `0.0000` MAE), `HDRCubeMap` (FP16+ST.2084+Array SRV), `VulkanMeshShowcase` (`XS2 ` `AS`+`MS`+`PS`+3D LUT `0.0000` MAE) | RTX 2060 SUPER & RTX 3080 Ti (`DXR=12`), AMD RX 7600/7900 XTX/9070 XT (`DXR=11`), GTX 1650 Ti (`MeshTier=10`, Vk 1.4.329 `VK_EXT_mesh_shader`) + WARP (`test_dxr_live`, `test_gdk_simple_pbr`, `test_gdk_meshlet_cull`, `test_gdk_dxr_triangle`, `test_gdk_simple_lighting`, `test_gdk_hdr_cubemap`, `test_vulkan_mesh_showcase`) |
| **Retail DRM / Encrypted `.xvc`/`.xvd` Decryption** | `Legal Boundary` | 🚫 Out of Scope | `0%` | Intentionally excluded — OpenScarlett is strictly clean-room and never extracts keys or bypasses DRM | N/A (Permanent Policy) |

_Auto-generated from live CTest results via `python tools/generate_status.py --build-dir build`._
<!-- OPENSCARLETT_STATUS_END -->

---

## How OpenScarlett Works (Plain English)

```text
  Unpacked Xbox GDK / Series X|S Package (.exe + MicrosoftGame.Config)
        │
        ▼
  [1. Loader & Live Telemetry]   Launches .exe suspended -> injects hook DLL
        │                        Redirects Xbox DLLs + logs every API / COM slot hit
        ▼
  [2. Platform & GDK Shims]      kernelx.dll (64KB/4MB memory aliasing + GPU Tile Pool)
        │                        xgameruntime, GameInput, XAudio2/AcpHal, dstorage_xs, winrt_xs, XSAPI
        ▼
  [3. Media & Storage Codecs]    Real GDeflate v1 (0x04/0xFB), Xbox BCPack (BC1-BC7),
        │                        XMA2 IMDCT audio, Bink2 video, GPU FSR 1.0 (EASU + RCAS)
        ▼
  [4. Graphics & RDNA2 Lifter]   d3d12_xs COM proxy + RDNA2/GFX10.3 -> SPIR-V 1.3 & DXIL (SM6.5)
        │                        (VS, PS, CS, GS, Mesh/Amplification Shaders, MIMG, LDS, Wave64)
        ▼
  Native Windows 10 / 11 PC GPU  (DirectX 12 + Vulkan 1.2/1.4 + WARP software fallback)
```

### Why We Test Against Real Binaries & Real GPUs (Zero Guesswork)
Many early console translation layers pass small synthetic unit tests but immediately crash on real titles. OpenScarlett separates **Unit-Tested** code from **Live-GPU / Real-Binary Verified** code:
1. **Real Hardware Probe & Execution:** Tests run on **6 real PCs** across both NVIDIA and AMD — verified on **Win10 + GTX 1650 Ti** (`SM 6.5`, `Vk 1.4.329`), **Win11 26H2 + RTX 2060 SUPER** (`DXR=12`, `Vk 1.4.351`), **Win11 25H2 + RTX 3080 Ti** (`DXR=12`, `Vk 1.4.357`), **Win11 26H2 + AMD RX 7600** (`Wave64`, `DXR=11`, `Vk 1.4.359`), **Win11 26H2 + AMD RX 7900 XTX** (`Wave64`, `DXR=11`, `Vk 1.4.359`), **Win11 26H2 + AMD RX 9070 XT** (`Wave64`, `DXR=11`, `Vk 1.4.315`), and **D3D12 WARP**.
2. **Live Call & COM Vtable Telemetry:** Every DLL import and `d3d12_xs` COM vtable slot called by a real binary is logged to JSON so unimplemented blockers jump straight to the top of the report.
3. **Strict Shader Validation:** Lifted RDNA2 shaders are validated against Khronos `spirv-val` and Microsoft `dxc.exe` (Shader Model 6.5/6.6 DXIL) before GPU execution and pixel readback checks.

---

## Real-Binary & Live-GPU Compatibility Matrix

All live measurements below were recorded on **Windows 10 x64** with **NVIDIA GeForce GTX 1650 Ti** (`3935 MB` VRAM, `SM 6.5`, **Hardware Mesh Shaders `MeshTier=10`**, `Wave=32`, `DXR=0`, **Vulkan `1.4.329`**) and **D3D12 WARP** (`SM 6.2`). Every GDK sample executed as a standalone Xbox-ABI PE32+ executable inside an isolated directory with **zero shim DLLs copied next to the `.exe`**, launched via `openscarlett.exe run` with suspended-process PE IAT redirection + `openscarlett_hook.dll` APC injection.

| Target / Workload | Type | Adapter | Result | Measured Output / FNV-1a Frame Hash | GPU Frame | Live `CallTelemetry` Blockers |
| :--- | :--- | :--- | :---: | :--- | :---: | :---: |
| **Microsoft `Xbox-GDK-Samples` / `SimpleTriangle`** | Real GDK Binary (`openscarlett run`) | **NVIDIA GTX 1650 Ti** (`SM 6.5`) | 🟢 **Live Pass** | `256×256`, 8,192 RGB tri px, `0xB8E29520F579D065` | `3.51 ms` | `0` (`68` calls / `47` symbols; `DrawInstanced` #12, `PresentX` #19) |
| **Microsoft `Xbox-GDK-Samples` / `SimpleTriangle`** | Real GDK Binary (`openscarlett run`) | **D3D12 WARP** (`SM 6.2`) | 🟢 **Live Pass** | `256×256`, **bit-exact HW match** `0xB8E29520F579D065` | `3.08 ms` | `0` (`68` calls / `47` symbols) |
| **Microsoft `Xbox-GDK-Samples` / `SimpleMeshShader`** | Real GDK Binary (`ms_6_5` + `ps_6_5`) | **NVIDIA GTX 1650 Ti** (`MeshTier=10`) | 🟢 **Live Pass** | `256×256`, 8,192 RGB tri px, `0x617ACFC269023CA5` | `2.83 ms` | `0` (`57` calls / `38` symbols; `DispatchMesh` #79, `CreatePipelineState` #47) |
| **Microsoft `Xbox-GDK-Samples` / `SimpleCompute`** | Real GDK Binary (`fractal.hlsl` Mandelbrot) | **NVIDIA GTX 1650 Ti** (`cs_6_5`) | 🟢 **Live Pass** | `256×256`, 50,528 col / 15,008 set / 32 bands, `0x7062D0C262252A26` | `3.40 ms` | `0` (`66` calls / `41` symbols; `Dispatch` #14, `PresentX` #19) |
| **Microsoft `Xbox-GDK-Samples` / `SimpleCompute`** | Real GDK Binary (`fractal.hlsl` Mandelbrot) | **D3D12 WARP** (`cs_6_2`) | 🟢 **Live Pass** | `256×256`, 50,522 col / 15,014 set / 32 bands, `0x0598B92DDCF7137A` | `5.73 ms` | `0` (`66` calls / `41` symbols) |
| **Multi-Frame 3D `SimplePBR`** (`4× MSAA`, `D32_FLOAT` depth, PBR textures, CBs, `ResolveSubresource`) | Real GDK Binary (`openscarlett run <pkg>`) | **NVIDIA GTX 1650 Ti** (`SM 6.5`) | 🟢 **Live Pass** | `256×256`, 1,280 tris, 3 animated frames (`F2=0xDC2F8F3277DA04C8`), 4,527 RGB colors | `1.02 ms` (`7.05 ms` / 3f) | `0` (`188` calls / `68` symbols; `ResolveSubresource` #19, `ClearDSV` #47) |
| **Multi-Frame 3D `SimplePBR`** | Real GDK Binary (`openscarlett run <pkg>`) | **D3D12 WARP** (`SM 6.2`) | 🟢 **Live Pass** | `256×256`, **HW vs WARP `MAE = 0.0264 / 255`**, exact 33,751 non-BG coverage (`F2=0x3CD31817D893C778`) | `9.80 ms` (`29.41 ms` / 3f) | `0` (`188` calls / `68` symbols) |
| **`MeshletCull` (`as_6_5` + `ms_6_5` + `ps_6_5` GPU Frustum + Normal-Cone Culling)** | Real GDK Binary (`openscarlett run <pkg>`) | **NVIDIA GTX 1650 Ti** (`MeshTier=10`) | 🟢 **Live Pass** | `256×256`, **6/16 surviving · 6 frustum · 4 cone culled**, `0x1203151701C9DE97` | `4.62 ms` | `0` (`86` calls / `54` symbols; `DispatchMesh` #79, `CreatePipelineState` #47) |
| **`MeshletCull` (`cs_6_2` + `vs_6_2`/`ps_6_2` Fallback)** | Real GDK Binary (`openscarlett run <pkg>`) | **D3D12 WARP** (`MeshTier=0`) | 🟢 **Live Pass** | `256×256`, **exact 6/6/4 culling & 6,534 px coverage**, HW vs WARP **`MaxDiff = 1 LSB`** (`0x208D800A4E2D13C8`) | `15.69 ms` | `0` (`99` calls / `60` symbols; `Dispatch` #14, `DrawInstanced` #12) |
| **`DXRTriangle` (`lib_6_3` HW RT / `XboxBvhBuilder` SAH BVH)** | Real GDK Binary (`openscarlett run <pkg>`) | **GTX 1650 Ti (`DXR=0` SW) & WARP** | 🟢 **Live Pass** | `256×256`, 10,112 hits / 10,113 colors, **bit-exact HW == WARP `0xE52BF8857986B576`** (native `lib_6_3` RT-core path ready for `DXR≥10`) | `63.74 ms` (SW) | `0` (`65,613` calls / `49` symbols; `XboxBvhBuilder` + `PresentX` #19) |
| **`SimpleLighting` (DirectXTK12-style 3D Blinn-Phong + `D32F` + `XDisplay` + `X3DAudio`/`XAudio2FX`)** | Real GDK Binary (`openscarlett run <pkg>`) + Live Vulkan 1.4 | **GTX 1650 Ti (D3D12 + Vulkan 1.4.329) & WARP** | 🟢 **Live Pass** | `256×256`, 812 tris, 3 frames (`F2=0x6FB175EE056DF0BF`), **Vulkan 1.4 vs D3D12 HW bit-exact `MAE = 0.0000` (`max=0`)**; HW vs WARP `MAE = 0.0193`; `X3DAudio` Doppler `1.066→0.942` | `4.14 ms` (D3D12) / `13.49 ms` (Vk) | `0` (`184` calls / `83` symbols across `d3d12_xs`, `xgameruntime`, `XAudio2_9`, `pixevt`, `vfs`) |
| **`HDRCubeMap` (`FP16 TextureCube` + Default `Texture2DArray` SRV + HDR10 ST.2084 `PresentX` + ACES/Reinhard Fallback)** | Real GDK Binary (`openscarlett run <pkg>`) | **GTX 1650 Ti (HDR10 & SDR) & WARP** | 🟢 **Live Pass** | `256×256`, **6/6 Cube == 6/6 Default Array SRV (`0.00 nits` diff)**, ST.2084 peak `666.7 nits`, ACES (`0xD8C3…`) + Reinhard (`0x0A80…`), HW vs WARP `MAE = 0.0346 / 255` | `210.0 ms` (HW) | `0` (`175` calls / `64` symbols; `CreateShaderResourceView[DefaultArrayPreserved]` #18) |
| **`VulkanMeshShowcase` (Raw RDNA2 `XS2\0` `AS`+`MS`+`PS` + `32³` 3D LUT Texture)** | Real GDK Binary + Live `VK_EXT_mesh_shader` | **GTX 1650 Ti (`Vulkan 1.4.329` `task+mesh` & D3D12 `MeshTier=10`) + WARP** | 🟢 **Live Pass** | `256×256`, 19,800 px / 1,663 graded colors, `32³` 3D LUT bit-exact, **Vulkan 1.4 `VK_EXT_mesh_shader` vs D3D12 `MeshTier=10` `MAE = 0.0000` (`100.000%` match)** | `7.04 ms` (Vk) / `3.37 ms` (D3D12) | `0` (`82`–`91` calls across `d3d12_xs`, `xgameruntime`, `pixevt`, `vfs`; `--backend=vulkan` verified) |
| **Full Zero-SDK Vulkan 1.2/1.4 Graphics, `VK_EXT_mesh_shader`, 3D Textures & Win32 Swapchain** | Live Vulkan Graphics (`test_vulkan_graphics_live`) | **NVIDIA GTX 1650 Ti (`Vulkan 1.4.329`) + SW Fallback** | 🟢 **Live Pass** | Lifted RDNA2 VS+PS (`0x9E4D…`), 3D textured + `D32F` + 4× MSAA resolve + `FP16` (`0x5C7A…`), mipmapped 2D (`64²`) & 3D (`32³`) bit-exact, HW `Task+Mesh+PS` (`0x65B6…`), Win32 swapchain + HDR10 probe | — | `0` (`test_vulkan_graphics_live`) |
| **HDR10 ST.2084 & scRGB `PresentX`, Default Array SRVs, Unified-Memory Remap & Fail-Closed Stubs** | Live D3D12 Proxy (`test_hdr10_d3d12`) | **GTX 1650 Ti + D3D12 WARP** | 🟢 **Live Pass** | PQ ST.2084 round-trip `< 1e-4`, `R10G10B10A2` + `G2084_P2020`, `FP16` scRGB + ACES/Reinhard fallback, 4-slice default `Texture2DArray` SRV (`ScarlettX #46`), placed unified-memory VA remap flush, explicit `E_NOTIMPL` stubs | — | `0` (`test_hdr10_d3d12`) |
| **Untouched MIT `Microsoft Xbox-GDK-Samples` / `SimpleDirectStorageCombo`** (5 untouched `.cpp` + `.mgc`) | Untouched External MIT Binary (`openscarlett run <pkg>`) | **Windows 10 x64** | 🟢 **Live Pass** | `SimpleLoad`, `StatusBatch`, `CompletionEvent`, `MultipleQueues`, CoW pkg `0x6824D2954256492C` byte-identical | — | `0` (`769` calls / `17` symbols across `dstorage_xs` + `vfs`) |
| **19-DLL Full-Stack External GDK Binary** (all 19 Xbox/GDK DLLs + `QueryApiImpl` + `XS2\0` auto-lift + CoW VFS) | External GDK-Header Binary (`openscarlett run <pkg>`) | **GTX 1650 Ti + WARP** | 🟢 **Live Pass** | **19/19 shims live**, cold cache `miss=1,mem=1,disk=0` → warm **`miss=0,mem=1,disk=1`**, pkg `0xB0E6785A25C7B020` 100% byte-identical | — | `0` (`546` calls / `265` symbols cold; `543` calls / `262` symbols warm) |
| **10-DLL GDK `SystemCombo`** (`kernelx` + `xgameruntime` + `GameInput` + `dstorage_xs` + `XAudio2_9`/`AcpHal` + `xg_xs` + `winrt_xs` + `XSAPI`) | Real GDK Binary (`openscarlett run`) | **Windows 10 x64 + GTX 1650 Ti** | 🟢 **Live Pass** | 64 KiB VA aliasing, disk `XGameSave`, GDeflate+BCPack, XMA2 mix, `64KB_S`, WinRT, XSAPI | — | `0` (`218` calls / `118` symbols across all 10 Xbox DLLs + `vfs`) |
| **RDNA2 AAA Lifter, CPU Interpreter & `d3d12_xs` Auto-Lift** (`permlane16`, `BFE`/`BFI`, VOP3P `f16`/`u16`, 3D/cube mip+atomics, `SCRATCH`, `BVH_INTERSECT_RAY`, Xbox DXBC, disk cache + async PSO) | Live Dual-GPU (`spirv-val` + `dxc` SM 6.5) | **GTX 1650 Ti (Vulkan 1.4 & D3D12 `MeshTier=10`) + WARP** | 🟢 **Live Pass** | `spirv-val` (`vulkan1.1`/`1.2`) PASS; **100% bit-exact CPU wave32/64 interpreter parity**; `XS2\0` CS/VS+PS/MS+PS auto-lift + `DiskHit=1` + async stand-in PSO (`2.08 ms`) | — | `0` (`test_rdna2_lifter_live`, `test_d3d12_xs_autolift`) |
| **Interactive Win32 `PresentX` Windowing & `GameInput` Loop** (`60`/`120 Hz` pacing, waitable latency, Alt+Enter borderless, resize/minimize/close) | Live Swapchain (`test_presentx_windowing`) | **GTX 1650 Ti + WARP** | 🟢 **Live Pass** | 6 animated frames bit-exact HW == WARP (`0x02E9…`→`0x6FB0…`); `120 Hz` (`25.8 ms`/4f) & `60 Hz` (`50.1 ms`/3f, `16.67 ms`/f); waitable `0x840` | `16.67 ms` (paced) | `0` (`test_presentx_windowing`) |
| **Opus MFT (`0x704F`), IMA-ADPCM & Live WASAPI + Full GFX10 `xg_xs`** (15 2D + 6 3D thick + mip tails + 19 formats) | Live Host Audio & Tiling (`test_audio_opus_wasapi`, `test_xg_dstorage`) | **Windows 10 x64** | 🟢 **Live Pass** | Native `MSOpusDecoder.dll` (`7,680` frames), IMA-ADPCM `37 dB` SNR, event-driven WASAPI `48 kHz 2ch` (`4,896` frames), 80 2D + 3D/mip-tail/cube/BC1–BC7/depth roundtrips | — | `0` (`test_audio_opus_wasapi`, `test_xg_dstorage`) |
| **GPU FSR 1.0 (`EASU + RCAS`) via FidelityFX Xbox Shims** (`amd_fidelityfx_loader_xbox` → `upscaler`) | Live GPU (`test_d3d12_xs_live`, `test_secondary_shims`) | **GTX 1650 Ti, RTX 2060 SUPER/3080 Ti, AMD RX 7600/7900 XTX/9070 XT & WARP** | 🟢 **Live Pass** | `64×64 → 128×128`: **AMD RDNA3/RDNA4 `MaxDiff = 0 LSB`** vs WARP (`0x68799DB54205F090`); NVIDIA Turing/Ampere `MaxDiff = 1 LSB`; `Bink2Scarlett` `0.67 LSB` MAE | — | `0` (`test_d3d12_xs_live`, `test_secondary_shims`) |
| **Hardware DXR Tier 1.1 (`hardware-DXR`)** | Live HW RT Cores (`test_dxr_live`) | **RTX 2060 SUPER & RTX 3080 Ti (`DXR=12`), AMD RX 7600 / RX 7900 XTX / RX 9070 XT (`DXR=11`)** | 🟢 **Live HW Pass** | `160×120`, `92.82%` hit rate, `device5=yes`, `inline_rt=yes`, exact `0x86443514BF44B806` vs WARP across NVIDIA Turing/Ampere + AMD RDNA3/RDNA4 | — | `0` (verified in [#1](https://github.com/californiaAlmonds/OpenScarlett/issues/1), [#8](https://github.com/californiaAlmonds/OpenScarlett/issues/8), [#9](https://github.com/californiaAlmonds/OpenScarlett/issues/9), [#10](https://github.com/californiaAlmonds/OpenScarlett/issues/10), [#12](https://github.com/californiaAlmonds/OpenScarlett/issues/12), [#13](https://github.com/californiaAlmonds/OpenScarlett/issues/13)) |
| **Hardware DXR Tier 1.1 (SAH Software BVH Fallback)** | Probed GPU + SW Fallback (`test_dxr_live`) | **GTX 1650 Ti (`DXR=0`) + WARP** | 🟣 **SW Fallback Pass** | `160×120`, `92.82%` hit rate, GPU-uploaded 64B SBT, exact `0x86443514BF44B806` HW vs WARP | — | `0` (`test_dxr_live`) |

---

## Community-Verified Hardware & Contributors

| PC Hardware (GPU & CPU) | Windows Build | Probed Capabilities | Self-Test Result | Reported By |
| :--- | :--- | :--- | :---: | :--- |
| **NVIDIA GeForce GTX 1650 Ti** (`4 GB`, Turing) | Windows 10 22H2 (`19045.3086`) | `SM 6.5` · `Wave32` · `MeshTier=10` · `DXR=0` · `Vulkan 1.4.329` (`VK_EXT_mesh_shader=task+mesh`) | 🟢 **29/29 Pass** | Maintainer (Dev Host) |
| **NVIDIA GeForce RTX 2060 SUPER** (`8 GB`, Turing) + Ryzen 5 3600XT | Windows 11 26H2 (`26300.9974`) | `SM 6.6` · `Wave32` · `MeshTier=10` · **`DXR=12` (HW RT)** · `Vulkan 1.4.351` | 🟢 **13/13 Live Pass** | [@rpatty-dev](https://github.com/rpatty-dev) ([#1](https://github.com/californiaAlmonds/OpenScarlett/issues/1), [#8](https://github.com/californiaAlmonds/OpenScarlett/issues/8)) |
| **NVIDIA GeForce RTX 3080 Ti** (`12 GB`, Ampere · GeForce 621.01) | Windows 11 25H2 (`26200.9974`) | `SM 6.6` · `Wave32` · `MeshTier=10` · **`DXR=12` (HW RT)** · `Vulkan 1.4.357` | 🟢 **13/13 Live Pass** | [@sackboysplayer](https://github.com/sackboysplayer) ([#13](https://github.com/californiaAlmonds/OpenScarlett/issues/13)) |
| **AMD Radeon RX 7600** (`8 GB`, RDNA3 Navi 33) | Windows 11 26H2 (`26300.9711`) | `SM 6.6` · **`Wave=32..64`** · `MeshTier=10` · **`DXR=11` (HW RT)** · `Vulkan 1.4.359` | 🟢 **13/13 Live Pass** | [@AntoninMichelq](https://github.com/AntoninMichelq) ([#12](https://github.com/californiaAlmonds/OpenScarlett/issues/12)) |
| **AMD Radeon RX 7900 XTX** (`24 GB`, RDNA3 Navi 31) | Windows 11 26H2 (`26300.10075`) | `SM 6.6` · **`Wave=32..64`** · `MeshTier=10` · **`DXR=11` (HW RT)** · `Vulkan 1.4.359` | 🟢 **13/13 Live Pass** | [@wellgamer789](https://github.com/wellgamer789) ([#9](https://github.com/californiaAlmonds/OpenScarlett/issues/9)) |
| **AMD Radeon RX 9070 XT** (`16 GB`, RDNA4 Navi 48) | Windows 11 26H2 (`26300.9974`) | `SM 6.6` · **`Wave=32..64`** · `MeshTier=10` · **`DXR=11` (HW RT)** · `Vulkan 1.4.315` | 🟢 **13/13 Live Pass** | [@silentbay22](https://github.com/silentbay22) ([#10](https://github.com/californiaAlmonds/OpenScarlett/issues/10)) |
| **AMD Radeon RX 6000 (RDNA2) & Handheld APUs** | Windows 10 / 11 | Native `Wave64` · `Mesh` · `DXR` · UMA APUs | 🟡 *Tester Needed!* | [Download & Test →](https://github.com/californiaAlmonds/OpenScarlett/releases/latest) |
| **Intel Arc (A/B-Series) & Integrated GPUs** | Windows 10 / 11 | `Wave8`/`Wave16`/`Wave32` · UMA Heaps | 🟡 *Tester Needed!* | [Download & Test →](https://github.com/californiaAlmonds/OpenScarlett/releases/latest) |

### 🙏 Thank You to Our Contributors & Testers!
A huge thank you to everyone helping test, break, and improve OpenScarlett on real PC hardware!
- **[@rpatty-dev](https://github.com/rpatty-dev)** ([#1](https://github.com/californiaAlmonds/OpenScarlett/issues/1), [#6](https://github.com/californiaAlmonds/OpenScarlett/issues/6), [#8](https://github.com/californiaAlmonds/OpenScarlett/issues/8), **PR [#11](https://github.com/californiaAlmonds/OpenScarlett/pull/11)**) — First community hardware tester **and** first merged community PR! Validated **13/13 live tests (`v0.2.1-alpha`, 0 blockers)** on **NVIDIA RTX 2060 SUPER + Ryzen 5 3600XT / Win11 26H2** (`DXR=12`, `Vulkan 1.4.351`), pinpointed the `d3d12_xs` MinGW-w64 GCC 16.2 compatibility blockers, and contributed **120 NPOT mip-chain + 24 32×32×32 3D LUT** `xg_xs` swizzle test vectors (closing [#4](https://github.com/californiaAlmonds/OpenScarlett/issues/4)).
- **[@wellgamer789](https://github.com/wellgamer789)** ([#9](https://github.com/californiaAlmonds/OpenScarlett/issues/9)) — First **AMD RDNA3 / native Wave64 (`Wave=32..64`)** hardware validation! Passed **13/13 live tests (0 blockers)** on **AMD Radeon RX 7900 XTX (`24 GB`) / Win11 26H2 (`26300.10075`)** with live **`hardware-DXR` Tier 11**, **Vulkan 1.4.359**, `AS+MS+PS DispatchMesh`, and **bit-exact `0 LSB` FSR 1.0** parity vs WARP.
- **[@silentbay22](https://github.com/silentbay22)** ([#10](https://github.com/californiaAlmonds/OpenScarlett/issues/10)) — First **AMD RDNA4 / native Wave64 (`Wave=32..64`)** hardware validation! Passed **13/13 live tests (0 blockers)** on **AMD Radeon RX 9070 XT (`16 GB`) / Win11 26H2 (`26300.9974`)** with live **`hardware-DXR` Tier 11**, **Vulkan 1.4.315**, `AS+MS+PS DispatchMesh`, and **bit-exact `0 LSB` FSR 1.0** parity vs RX 7900 XTX & WARP.
- **[@AntoninMichelq](https://github.com/AntoninMichelq)** ([#12](https://github.com/californiaAlmonds/OpenScarlett/issues/12)) — Validated **13/13 live tests (0 blockers)** on **AMD Radeon RX 7600 (`8 GB`, RDNA3 Navi 33) / Win11 26H2 (`26300.9711`)** with native `Wave=32..64`, live **`hardware-DXR` Tier 11**, **Vulkan 1.4.359**, and discrete-over-iGPU selection.
- **[@sackboysplayer](https://github.com/sackboysplayer)** ([#13](https://github.com/californiaAlmonds/OpenScarlett/issues/13)) — First **NVIDIA RTX 30-series (Ampere)** and **Windows 11 25H2 (`26200.9974`)** validation! Passed **13/13 live tests (0 blockers)** on **NVIDIA GeForce RTX 3080 Ti (`12 GB`, GeForce 621.01)** with live **`hardware-DXR` Tier 12**, **Vulkan 1.4.357**, `AS+MS+PS DispatchMesh`, and `1.63 ms` `SimpleTriangle` GPU frame time.

Want your GPU added to the table above? **[Grab the latest 60-second test bundle](https://github.com/californiaAlmonds/OpenScarlett/releases/latest)** and submit a report!

---

## Quick Start (Build from Source — Windows 10 / 11 x64)

### Requirements
- Windows 10 (1903+) or Windows 11 (x64)
- Visual Studio 2022 (MSVC C++20) + Windows 10/11 SDK (with `dxc.exe`)
- CMake 3.20+ and Ninja

### Build & Run Tests
```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Try the CLI Right Now
```powershell
# Inspect your PC's D3D12, DXR, Mesh Shader, Wave lane, Vulkan, and DXC support
.\build\openscarlett.exe probe-gpu

# Inspect any x64 .exe or .dll to see its Xbox / GDK / Windows imports & exports
.\build\openscarlett.exe inspect path\to\binary.exe

# Launch an unpacked Xbox GDK / Series X|S package folder (auto-reading MicrosoftGame.Config,
# CoW read-only G:/GAME: overlay, per-TitleId S: saves, optional --backend=vulkan, and unified CallTelemetry JSON report)
.\build\openscarlett.exe run path\to\package_dir_or_game.exe --telemetry report.json
.\build\openscarlett.exe run --backend=vulkan path\to\package_dir_or_game.exe
```

---

## Roadmap

- [x] **Bootstrap (`v0.1`)**: MIT clean-room foundation, PE32+ import/delay-load inspector, live call telemetry, GDeflate v1, BCPack, XMA2 IMDCT, Bink2, WinRT `HSTRING`/activation, RDNA2 decoder + SSA IR, live Vulkan 1.4 GPU compute execution.
- [x] **Sprint 1 — Live Loader & Core Shims**: Suspended-process APC hook loader (`openscarlett_hook.dll` + `openscarlett run`) with pre-loader PE IAT redirection, virtual `G:\`/`T:\`/`S:\` mounts, `kernelx.dll` (41 exports) + 64 KiB/4 MiB `VirtualAlloc2`/`MapViewOfFile3` aliasing + `GpuTilePool`, modular `d3d12_xs.dll` COM proxies + `xg_xs.dll` deswizzle + GPU FSR 1.0, and 6 core GDK shims (`xgameruntime`, `GameInput`, `XAudio2_9`/`AcpHal`, `dstorage_xs`, `XSAPI`).
- [x] **Sprint 2 — AAA RDNA2 Shader Lifter & DXR 1.1**: `MIMG` texture ops, `DS` LDS shared memory + atomics + `S_BARRIER`, `wave64` → `wave32` emulation, Mesh/Amplification shader (`MS`/`AS`) lifting, validated via Khronos `spirv-val` + Vulkan 1.4 + Microsoft `dxc.exe` SM 6.5 on live GTX 1650 Ti (`MeshTier=10`) & WARP, plus `ID3D12Device5` DXR Tier 1.1 + SAH BVH fallback.
- [x] **Sprint 3 — Real Live GDK Binaries (`Xbox-GDK-Samples`)**: End-to-end automated bring-up running official MIT **Microsoft `Xbox-GDK-Samples`** (`SimpleTriangle`, `SimpleMeshShader`, `SimpleCompute` Mandelbrot, plus full 10-DLL `SystemCombo`) live through `openscarlett run` from zero-DLL package folders with frame readback hashes and `not_impl = 0` telemetry.
- [x] **Sprint 4 — AAA Shader Cache/Auto-Lift, Full 19-DLL Stack, Interactive `PresentX` & Multi-Pass GDK Bring-Up**: AAA RDNA2 opcodes (`permlane16`/`x16`, `BFE`/`BFI`, VOP3P `f16`/`u16`, 3D/cube mip+atomics, `SCRATCH`, `BVH_INTERSECT_RAY`, Xbox DXBC), wave32/64 CPU reference interpreter, content-hashed disk shader cache + async `dxc` PSO + `d3d12_xs` auto-lifting; `MSOpusDecoder.dll` MFT Opus (`0x704F`) + IMA-ADPCM + live WASAPI; all 15 GFX10 2D & 6 3D thick `xg_xs` modes + mip tails; `MicrosoftGame.Config` package loader + read-only CoW overlay VFS + PE forwarders/TLS/`.pdata` SEH + 19-DLL routing; `xgameruntime` `QueryApiImpl` + 9 secondary shim DLLs (`pixevt`, `xmem`, `xcrdapi`, `libHttpClient.GDK`, `XCurl`, `Bink2Scarlett`, FidelityFX loader/upscaler, `XboxBvhBuilder`); interactive Win32 `PresentX` (60/120 Hz pacing, waitable latency, Alt+Enter borderless, resize/close); and live multi-pass `SimplePBR`, `MeshletCull`, `DXRTriangle`, untouched MIT `SimpleDirectStorageCombo`, and 19-DLL full-stack bring-up (`23/23` CTest, `not_impl = 0`).
- [x] **Sprint 5 (`v0.3.0-alpha`) — Full Vulkan 1.4 Graphics & Mesh Shaders, HDR10, Ecosystem Hardening & Cross-API Real-Binary Bring-Up**: Full zero-SDK Vulkan 1.2/1.4 rasterization (`RGBA8`/`FP16`/`D32F`/4× MSAA resolve), native `VK_EXT_mesh_shader` (`Task+Mesh+PS`), mipmapped 2D/3D volume textures, Win32 swapchain + HDR10 probe, and `--backend=vulkan` routing; 5 RDNA2 ISA & Wave64 ecosystem catches (zero-init LDS, exact `f16` FMA vs denorm-flushing MAD, encoding-size 16-bit `VOP3`, `s_bcnt1` b32/b64 high-word fix, helper-lane quad `ddx`/`ddy`, bounded `wave64`-on-`wave32` mesh dispatcher, WARP `1.0.22` guard); HDR10 ST.2084 & scRGB `PresentX` with ACES/Reinhard SDR tone-map fallback, default `Texture2DArray` SRV slice preservation, unified-memory placed texture remap tracking, and fail-closed `E_NOTIMPL` stubs; `XDisplay` HDR, `XLauncher`/`XGameUI`, `XGameInvite`, `XAccessibility`/`XSpeechSynthesizer`, `X3DAudio`/`XAudio2FX`, and Kahn topological DLL init ordering; Clang-CL & MinGW-w64 CI legs; and live `SimpleLighting` (`0.0000` MAE Vulkan 1.4 vs D3D12), `HDRCubeMap`, and `VulkanMeshShowcase` (`0.0000` MAE `VK_EXT_mesh_shader` vs D3D12 `MeshTier=10`) (`29/29` CTest, `not_impl = 0`).
- [ ] **Sprint 6 — Larger Open-Source GDK Titles, Full `d3d12_xs`→Vulkan Routing & Broad Multi-Vendor Validation (Next)**: Direct `d3d12_xs` command-list routing to the Vulkan 1.4 graphics backend for full title portability, larger open-source DirectXTK12/GDK titles, and community validation across AMD RDNA2/3/4, Intel Arc, and RTX 20/30/40/50 GPUs.

---

## Contributing & License

Hardware test reports, bug reports, and clean-room PRs are all welcome! See [`CONTRIBUTING.md`](CONTRIBUTING.md) for the 60-second hardware test guide and clean-room contribution rules, and [`AGENTS.md`](AGENTS.md) for engineering guidelines.

Licensed under the **[MIT License](LICENSE)**.
