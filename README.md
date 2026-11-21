# OpenScarlett

[![Windows CI](https://github.com/californiaAlmonds/OpenScarlett/actions/workflows/ci.yml/badge.svg)](https://github.com/californiaAlmonds/OpenScarlett/actions/workflows/ci.yml)
[![Latest Hardware Test Build](https://img.shields.io/github/v/release/californiaAlmonds/OpenScarlett?label=download%2060s%20test%20bundle&color=2ea043)](https://github.com/californiaAlmonds/OpenScarlett/releases/latest)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

**OpenScarlett** is an open-source, MIT-licensed **Xbox Series X|S (`Scarlett`) translation layer for Windows 10 & Windows 11 (x64)**.

Because Xbox Series X|S runs on an **AMD Zen 2 x86-64 CPU** and a **Windows-derived Game OS**, games do not need slow CPU emulation. Instead, OpenScarlett runs the game's native x86-64 instructions directly on your PC while translating the Xbox-specific graphics (`d3d12_xs`), precompiled RDNA2 GPU shaders, memory aliasing (`kernelx`), audio/media codecs, and GDK runtime APIs to native Windows DirectX 12 & Vulkan.

> [!IMPORTANT]
> ### 🧪 Help Us Test OpenScarlett on Your GPU — 60 Seconds, No Visual Studio Needed!
>
> OpenScarlett has only been tested on **2 real PCs** so far (**Win10 + GTX 1650 Ti** and **Win11 26H2 + RTX 2060 SUPER**). Every new GPU and Windows build helps us catch real driver, shader, and memory-aliasing quirks we cannot reproduce locally!
>
> **How to help in 3 clicks:**
> 1. **[⬇️ Download the latest `OpenScarlett-*-win64.zip`](https://github.com/californiaAlmonds/OpenScarlett/releases/latest)** and extract anywhere *(everything — including open-source `dxc.exe` — is bundled; zero coding or SDK required)*.
> 2. **Double-click `run_hardware_test.bat`** — runs all GPU/OS tests live with clear progress and automatically strips usernames & local folder paths.
> 3. **Press `Enter`** when prompted — opens a **pre-filled GitHub Hardware Report** in your browser. Just review and click **Create**! *(Failures on new hardware are just as valuable as passes!)*
>
> | Most Wanted Hardware | Why It Helps OpenScarlett |
> | :--- | :--- |
> | **AMD Radeon RX 6000 / 7000 / 9000 & Handheld APUs** *(ROG Ally, Legion Go, Steam Deck Windows)* | Native **Wave64** shaders (closest match to Xbox Series X\|S RDNA2) + AMD D3D12/Vulkan driver behavior |
> | **Intel Arc (A/B-Series) & Integrated GPUs** *(Xe / Vega / RDNA iGPU)* | Subgroup/wave-size edge cases (`Wave8`/`Wave16`/`Wave32`) + UMA memory heap handling |
> | **NVIDIA RTX 30 / 40 / 50 Series** | Broad **DXR Tier 1.1+** hardware RT-core & SM 6.6–6.8 validation across newer drivers |
> | **Windows 11 (23H2 / 24H2 / 25H2 / 26H2)** | `VirtualAlloc2` / `MapViewOfFile3` placeholder aliasing & newer D3D12 / WinRT runtimes |

---

> **Legal & Clean-Room Notice:** OpenScarlett is 100% clean-room MIT-licensed C++20 code. It contains **no** proprietary Microsoft/Xbox binaries, encryption keys, firmware, or copyrighted game assets, and **never decrypts retail `.xvc`/`.xvd` packages**. You must supply your own legally obtained, unencrypted GDK binaries or open-source samples.

---

<!-- OPENSCARLETT_STATUS_START -->
## Live Project Status

![OpenScarlett Status](docs/status.svg)

**Overall Clean-Room Readiness:** `100.0%` · **Automated Tests:** `13/13` passing · **Verified Hardware:** `Win10 + GTX 1650 Ti` · `Win11 26H2 + RTX 2060 SUPER (DXR Tier 12)`

| Subsystem | Area | Status | Progress | What Works Right Now | Verified On |
| :--- | :--- | :--- | :---: | :--- | :--- |
| **PE32+ Inspector, APC Hook Loader & Call Telemetry** | `Loader` | 🟢 Live-Binary Verified | `100%` | PE32+ import/delay-load inspector, suspended-process APC hook loader (`openscarlett_hook.dll` + `openscarlett run`), pre-loader PE IAT redirection, `G:/T:/S:` VFS, unified cross-DLL JSON telemetry | Windows 10 x64 (`test_pe_inspect`, `test_apc_loader`, `test_gdk_live`) |
| **Vulkan 1.2/1.4 Dynamic Compute Backend** | `Graphics` | 🟢 Live-GPU Verified | `100%` | Zero-SDK dynamic `vulkan-1.dll` loader, SPIR-V 1.3 validator, live GPU compute/LDS/wave64 pipelines + software fallback | GTX 1650 Ti (Vk 1.4.329) & RTX 2060 SUPER (Vk 1.4.351, Win11) |
| **Production Codecs & GPU Upscaler (GDeflate / BCPack / XMA2 / Bink2 / FSR 1.0)** | `Media & IO` | 🟢 Live-GPU Verified | `100%` | Microsoft GDeflate v1 (`0x04/0xFB`), Xbox BCPack (BC1–BC7), XMA2 2048B IMDCT audio, Bink2 `KB2i` DCT video, live D3D12 GPU FSR 1.0 EASU+RCAS | GTX 1650 Ti, RTX 2060 SUPER & WARP (`test_codecs`, `test_d3d12_xs_live`) |
| **RDNA2 / GFX10.3 Shader Decoder & Lifter** | `Shaders` | 🟢 Live-GPU Verified | `100%` | RDNA2 disassembler + SSA IR (const-fold/DCE) + SPIR-V 1.3 & DXIL SM 6.5 lifter (`MIMG`, `DS`/LDS + `S_BARRIER`, `wave64`→`wave32`, `VS`/`PS`/`CS`/`MS`/`AS`) | `spirv-val` + `dxc` SM 6.5 + GTX 1650 Ti & RTX 2060 SUPER (`MeshTier=10`) + WARP |
| **WinRT Activation & HSTRING (`winrt_xs.dll`)** | `Runtime` | 🟢 Live-Binary Verified | `100%` | 20 exports: `RoInitialize`, activation factories, zero-alloc `HSTRING`, Xbox `AnalyticsInfo`/`ApplicationData` + `CallTelemetry` | Win10 & Win11 26H2 (`test_winrt_xs`, `test_apc_loader`, `test_gdk_live`) |
| **Xbox Unified Memory, `GpuTilePool` & `kernelx.dll`** | `Memory` | 🟢 Live-Binary Verified | `100%` | Win10/11 `VirtualAlloc2`/`MapViewOfFile3` 64 KiB & 4 MiB physical-virtual aliasing, `GpuTilePool`, 41 `kernelx.dll` exports | Win10 & Win11 26H2 (`test_kernelx`, `test_gdk_live`) |
| **`d3d12_xs.dll` COM Proxy & `xg_xs.dll` Swizzle** | `Graphics` | 🟢 Live-GPU Verified | `100%` | Modular `DeviceX`/`CommandQueueX`/`CommandListX` COM proxies with per-slot telemetry, `PresentX` (DXGI flip-model + readback), `DispatchMesh`, `xg_xs` `4KB_S`/`64KB_S` | GTX 1650 Ti, RTX 2060 SUPER (`MeshTier=10`) & WARP |
| **GDK Runtime (`xgameruntime`, `GameInput`, `XAudio2_9`, `AcpHal`, `dstorage_xs`, `XSAPI`)** | `GDK Shims` | 🟢 Live-Binary Verified | `100%` | 113 exports across 6 DLLs: `XTaskQueue`/`XAsync`, `XUser`, disk `XGameSave`, live XInput+KB `GameInput`, `XAudio2`/`AcpHal` XMA2, `dstorage_xs` GDeflate+BCPack, `XSAPI` | Win10 & Win11 26H2 (`test_gdk_shims`, `Xbox-GDK-Samples`) |
| **Hardware DXR Tier 1.1 Pipeline** | `Graphics` | 🟢 Live-GPU Verified | `100%` | `ID3D12Device5` BLAS/TLAS/SBT ABI + live hardware RT-core path (`DXR=12`) + 12-bin SAH software BVH fallback | RTX 2060 SUPER (`DXR=12` HW) + GTX 1650 Ti (`DXR=0` SW) + WARP |
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
1. **Real Hardware Probe & Execution:** Tests run on real PC GPUs — verified on **Windows 10 + NVIDIA GeForce GTX 1650 Ti** (`D3D12 SM 6.5` + `Vulkan 1.4.329`), **Windows 11 26H2 + NVIDIA GeForce RTX 2060 SUPER** (`D3D12 SM 6.6`, **`DXR Tier 12`**, `Vulkan 1.4.351`), and **D3D12 WARP**.
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
| **10-DLL GDK `SystemCombo`** (`kernelx` + `xgameruntime` + `GameInput` + `dstorage_xs` + `XAudio2_9`/`AcpHal` + `xg_xs` + `winrt_xs` + `XSAPI`) | Real GDK Binary (`openscarlett run`) | **Windows 10 x64 + GTX 1650 Ti** | 🟢 **Live Pass** | 64 KiB VA aliasing, disk `XGameSave`, GDeflate+BCPack, XMA2 mix, `64KB_S`, WinRT, XSAPI | — | `0` (`218` calls / `118` symbols across all 10 Xbox DLLs + `vfs`) |
| **RDNA2 Lifter Corpus** (ALU/Branch, `DS` LDS + `S_BARRIER` + Atomic, `wave64`→`wave32`, `MIMG`, `AS`+`MS`+`PS`) | Live Dual-GPU (`spirv-val` + `dxc` SM 6.5) | **GTX 1650 Ti, RTX 2060 SUPER (Vulkan 1.4 & D3D12 `MeshTier=10`) + WARP** | 🟢 **Live Pass** | `spirv-val` (`vulkan1.1`/`1.2`) PASS; 64/64 LDS+wave64 lanes exact; `64×64` AS+MS+PS RT verified | — | `0` (`test_rdna2_lifter_live`) |
| **GPU FSR 1.0 (`EASU + RCAS`) & `xg_xs` Swizzle** | Live GPU (`test_d3d12_xs_live`) | **GTX 1650 Ti, RTX 2060 SUPER & WARP** | 🟢 **Live Pass** | `64×64 → 128×128` HW vs WARP `MaxDiff = 1 LSB`; `4KB_S`/`64KB_S` bit-exact (RGBA8 & BC7) | — | `0` (`test_d3d12_xs_live`) |
| **Hardware DXR Tier 1.1 (`hardware-DXR`)** | Live HW RT Cores (`test_dxr_live`) | **NVIDIA RTX 2060 SUPER** (`DXR=12`, Win11 26H2) | 🟢 **Live HW Pass** | `160×120`, `92.82%` hit rate, `device5=yes`, `inline_rt=yes`, exact `0x86443514BF44B806` vs WARP | — | `0` (verified by [@rpatty-dev](https://github.com/rpatty-dev) in [#1](https://github.com/californiaAlmonds/OpenScarlett/issues/1)) |
| **Hardware DXR Tier 1.1 (SAH Software BVH Fallback)** | Probed GPU + SW Fallback (`test_dxr_live`) | **GTX 1650 Ti (`DXR=0`) + WARP** | 🟣 **SW Fallback Pass** | `160×120`, `92.82%` hit rate, GPU-uploaded 64B SBT, exact `0x86443514BF44B806` HW vs WARP | — | `0` (`test_dxr_live`) |

---

## Community-Verified Hardware & Contributors

| PC Hardware (GPU & CPU) | Windows Build | Probed Capabilities | Self-Test Result | Reported By |
| :--- | :--- | :--- | :---: | :--- |
| **NVIDIA GeForce GTX 1650 Ti** (`4 GB`) | Windows 10 22H2 (`19045.3086`) | `SM 6.5` · `Wave32` · `MeshTier=10` · `DXR=0` · `Vulkan 1.4.329` | 🟢 **13/13 Pass** | Maintainer (Dev Host) |
| **NVIDIA GeForce RTX 2060 SUPER** (`8 GB`) + AMD Ryzen 5 3600XT | Windows 11 26H2 (`26300.9974`) | `SM 6.6` · `Wave32` · `MeshTier=10` · **`DXR=12` (HW RT)** · `Vulkan 1.4.351` | 🟢 **11/13 Live Pass**\* | [@rpatty-dev](https://github.com/rpatty-dev) ([#1](https://github.com/californiaAlmonds/OpenScarlett/issues/1)) |
| **AMD Radeon RX 6000 / 7000 / 9000 & Handheld APUs** | Windows 10 / 11 | Native `Wave64` · `Mesh` · `DXR` · `Vulkan` | 🟡 *Tester Needed!* | [Download & Test →](https://github.com/californiaAlmonds/OpenScarlett/releases/latest) |
| **Intel Arc (A/B-Series) & Integrated GPUs** | Windows 10 / 11 | `Wave8`/`Wave16`/`Wave32` · UMA Heaps | 🟡 *Tester Needed!* | [Download & Test →](https://github.com/californiaAlmonds/OpenScarlett/releases/latest) |

*\*On `v0.2.0-alpha`, all live GPU, DXR Tier 12 hardware ray tracing, RDNA2 shader lifting (`AS+MS+PS DispatchMesh`, LDS, `wave64→wave32`, `MIMG`), Vulkan 1.4.351, FSR 1.0, Win11 26H2 `VirtualAlloc2` memory aliasing, and GDK shim tests passed on RTX 2060 SUPER; the 2 failures (`test_apc_loader`, `test_gdk_live`) caught a hardcoded CI build-dir path in the test harness itself (queued for fix in the next release).*

### 🙏 Thank You to Our Contributors & Testers!
A huge thank you to everyone helping test, break, and improve OpenScarlett on real PC hardware!
- **[@rpatty-dev](https://github.com/rpatty-dev)** ([#1](https://github.com/californiaAlmonds/OpenScarlett/issues/1)) — Our very first community hardware tester! Validated **real hardware DXR Tier 1.1 (`tier=12`)**, **Vulkan 1.4.351**, **SM 6.6 Mesh Shaders**, and **Windows 11 26H2** on an **NVIDIA RTX 2060 SUPER + Ryzen 5 3600XT**, and immediately surfaced two portable-bundle test harness path bugs.

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

# Inspect any x64 .exe or .dll to see its Xbox / GDK / Windows imports
.\build\openscarlett.exe inspect path\to\binary.exe

# Launch an unmodified Xbox GDK / Series X|S executable with suspended-process APC injection,
# G:/T:/S: virtual drive mounts, and a unified cross-DLL CallTelemetry JSON blocker report
.\build\openscarlett.exe run path\to\game.exe --telemetry report.json
```

---

## Roadmap

- [x] **Bootstrap (`v0.1`)**: MIT clean-room foundation, PE32+ import/delay-load inspector, live call telemetry, GDeflate v1, BCPack, XMA2 IMDCT, Bink2, WinRT `HSTRING`/activation, RDNA2 decoder + SSA IR, live Vulkan 1.4 GPU compute execution.
- [x] **Sprint 1 — Live Loader & Core Shims**: Suspended-process APC hook loader (`openscarlett_hook.dll` + `openscarlett run`) with pre-loader PE IAT redirection, virtual `G:\`/`T:\`/`S:\` mounts, `kernelx.dll` (41 exports) + 64 KiB/4 MiB `VirtualAlloc2`/`MapViewOfFile3` aliasing + `GpuTilePool`, modular `d3d12_xs.dll` COM proxies + `xg_xs.dll` deswizzle + GPU FSR 1.0, and 6 core GDK shims (`xgameruntime`, `GameInput`, `XAudio2_9`/`AcpHal`, `dstorage_xs`, `XSAPI` — 113 exports).
- [x] **Sprint 2 — AAA RDNA2 Shader Lifter & DXR 1.1**: `MIMG` texture ops, `DS` LDS shared memory + atomics + `S_BARRIER`, `wave64` → `wave32` emulation, Mesh/Amplification shader (`MS`/`AS`) lifting, validated via Khronos `spirv-val` + Vulkan 1.4 + Microsoft `dxc.exe` SM 6.5 on live GTX 1650 Ti (`MeshTier=10`) & WARP, plus `ID3D12Device5` DXR Tier 1.1 + SAH BVH fallback.
- [x] **Sprint 3 — Real Live GDK Binaries (`Xbox-GDK-Samples`)**: End-to-end automated bring-up running official MIT **Microsoft `Xbox-GDK-Samples`** (`SimpleTriangle`, `SimpleMeshShader`, `SimpleCompute` Mandelbrot, plus full 10-DLL `SystemCombo`) live through `openscarlett run` from zero-DLL package folders with frame readback hashes and `not_impl = 0` telemetry.
- [ ] **Sprint 4 — Complex Multi-Pass Titles & RT-Core GPU Validation (Next)**: DirectXTK12 / multi-pass PBR & HDR GDK samples (`SimplePBR`, `MeshletCull`, `DXRTriangle`), full Vulkan graphics pipeline translation, and native RT-core validation on RTX / RDNA2 hardware.

---

## Contributing & License

Hardware test reports, bug reports, and clean-room PRs are all welcome! See [`CONTRIBUTING.md`](CONTRIBUTING.md) for the 60-second hardware test guide and clean-room contribution rules, and [`AGENTS.md`](AGENTS.md) for engineering guidelines.

Licensed under the **[MIT License](LICENSE)**.
