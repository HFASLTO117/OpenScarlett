# OpenScarlett

[![Windows CI](https://github.com/californiaAlmonds/OpenScarlett/actions/workflows/ci.yml/badge.svg)](https://github.com/californiaAlmonds/OpenScarlett/actions/workflows/ci.yml)

**OpenScarlett** is an open-source, MIT-licensed **Xbox Series X|S (`Scarlett`) translation layer for Windows 10 & Windows 11 (x64)**.

Because Xbox Series X|S runs on an **AMD Zen 2 x86-64 CPU** and a **Windows-derived Game OS**, games do not need slow CPU emulation. Instead, OpenScarlett runs the game's native x86-64 instructions directly on your PC while translating the Xbox-specific graphics (`d3d12_xs`), precompiled RDNA2 GPU shaders, memory aliasing (`kernelx`), audio/media codecs, and GDK runtime APIs to native Windows DirectX 12 & Vulkan.

> **Legal & Clean-Room Notice:** OpenScarlett is 100% clean-room MIT-licensed C++20 code. It contains **no** proprietary Microsoft/Xbox binaries, encryption keys, firmware, or copyrighted game assets, and **never decrypts retail `.xvc`/`.xvd` packages**. You must supply your own legally obtained, unencrypted GDK binaries or open-source samples.

---

<!-- OPENSCARLETT_STATUS_START -->
## Live Project Status

![OpenScarlett Status](docs/status.svg)

**Overall Clean-Room Readiness:** `47.2%` · **Automated Tests:** `7/7` passing · **Host Verified:** `Windows 10 x64 + NVIDIA GTX 1650 Ti (D3D12 + Vulkan 1.4.329)`

| Subsystem | Area | Status | Progress | What Works Right Now | Verified On |
| :--- | :--- | :--- | :---: | :--- | :--- |
| **PE32+ Inspector & Live Call Telemetry** | `Loader` | 🟢 Live-Binary Verified | `100%` | PE32+ import & delay-load classification, JSON vtable/call blocker telemetry; suspended-process APC hook loader + G:/T:/S: VFS | Windows 10 x64 (`test_pe_inspect`) |
| **Vulkan 1.2/1.4 Dynamic Compute Backend** | `Graphics` | 🟢 Live-GPU Verified | `100%` | Zero-SDK dynamic `vulkan-1.dll` loader, SPIR-V 1.3 validator, live GPU compute pipelines + bit-exact software fallback | NVIDIA GeForce GTX 1650 Ti (Vulkan 1.4.329, `test_vulkan_live`) |
| **Production Codecs (GDeflate / BCPack / XMA2 / Bink2)** | `Media & IO` | 🟣 Unit-Tested | `80%` | Microsoft GDeflate v1 (`0x04/0xFB`), Xbox BCPack (BC1–BC7), XMA2 2048B IMDCT audio, Bink2 `KB2i` DCT video | Windows 10 x64 (`test_codecs`) |
| **RDNA2 / GFX10.3 Shader Decoder & Lifter** | `Shaders` | 🟡 In Progress | `45%` | RDNA2/GFX10.3 binary decoder + CFG/SSA IR (constant folding + DCE); SPIR-V + DXC SM6.5/Mesh/Wave64 lifter next | Windows 10 x64 (`test_rdna2_core`) |
| **WinRT Activation & HSTRING (`winrt_xs.dll`)** | `Runtime` | 🟣 Unit-Tested | `100%` | 20 exports: `RoInitialize`, activation factories, zero-alloc `HSTRING`, Xbox `AnalyticsInfo`/`ApplicationData` | Windows 10 x64 (`test_winrt_xs`) |
| **Xbox Unified Memory, `GpuTilePool` & `kernelx.dll`** | `Memory` | ⚪ Planned | `0%` | Next sprint: Win10 `VirtualAlloc2`/`MapViewOfFile3` aliasing, 64 KiB `GpuTilePool`, `kernelx.dll` | Queued (Sprint 1) |
| **`d3d12_xs` Guest COM Proxy & `xg_xs` Swizzle** | `Graphics` | ⚪ Planned | `0%` | Next sprint: modular `device_x`/`queue_x`/`cmdlist_x` COM proxies with live slot telemetry + `xg_xs` | Queued (Sprint 1) |
| **GDK Runtime (`xgameruntime`, `GameInput`, `XAudio2`, `dstorage_xs`, `XSAPI`)** | `GDK Shims` | ⚪ Planned | `0%` | Next sprint: clean-room GDK DLL shims verified against live Microsoft `Xbox-GDK-Samples` | Queued (Sprint 1) |
| **Hardware DXR Tier 1.1 Pipeline** | `Graphics` | ⚪ Planned | `0%` | Queued (Sprint 2): ID3D12Device5 DXR 1.1 + software BVH fallback for GTX 1650 Ti / WARP | Queued (Sprint 2) |
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
1. **Real Hardware Probe & Execution:** Tests run on real PC GPUs (currently developed and tested on a **Windows 10 laptop with an NVIDIA GeForce GTX 1650 Ti** using both **D3D12** and **Vulkan 1.4.329**, plus **D3D12 WARP**).
2. **Live Call & COM Vtable Telemetry:** Every DLL import and `d3d12_xs` COM vtable slot called by a real binary is logged to JSON so unimplemented blockers jump straight to the top of the report.
3. **Strict Shader Validation:** Lifted RDNA2 shaders are validated against Khronos `spirv-val` and Microsoft `dxc.exe` (Shader Model 6.5/6.6 DXIL) before GPU execution and pixel readback checks.

---

## Real-Binary & Live-GPU Test Matrix

| Target | Type | Result | Hardware / Notes |
| :--- | :--- | :---: | :--- |
| **Vulkan 1.4 Compute Pipeline** | Live GPU (`test_vulkan_live`) | 🟢 **Passing** | Executed SPIR-V 1.3 compute shader live on **NVIDIA GTX 1650 Ti (Vulkan 1.4.329)** |
| **PE32+ Import & Telemetry Analyzer** | Live Binary (`test_pe_inspect`) | 🟢 **Passing** | Parses PE32+ standard + delay-load imports & exports JSON blocker reports |
| **Microsoft `Xbox-GDK-Samples`** | Real GDK C++ Titles | 🟡 **Next Sprint** | Wiring suspended-process APC hook loader + `d3d12_xs` / `xgameruntime` shims |
| **RDNA2 Mesh / Wave64 / MIMG Corpus** | Live GPU (`dxc` + `spirv-val`) | 🟡 **Next Sprint** | Extending RDNA2 decoder/IR to Mesh/Amplification shaders, `MIMG`, `DS` LDS, `wave64` |

---

## Quick Start (Windows 10 / 11 x64)

### Requirements
- Windows 10 (1903+) or Windows 11 (x64)
- Visual Studio 2022 (MSVC C++20) + Windows 10/11 SDK
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
```

---

## Roadmap

- [x] **Bootstrap (`v0.1`)**: MIT clean-room foundation, PE32+ import/delay-load inspector, live call telemetry, GDeflate v1, BCPack, XMA2 IMDCT, Bink2, WinRT `HSTRING`/activation, RDNA2 decoder + SSA IR, live Vulkan 1.4 GPU compute execution.
- [ ] **Sprint 1 — Live Loader & Core Shims**: Suspended-process APC hook loader (`openscarlett_hook.dll`), virtual `G:\`/`T:\`/`S:\` mounts, `kernelx.dll` + 64 KiB `GpuTilePool` + OS placeholder aliasing, modular `d3d12_xs` COM proxies + `xg_xs` deswizzle, `xgameruntime` + `GameInput` + `XAudio2`/`AcpHal` + `dstorage_xs`.
- [ ] **Sprint 2 — AAA RDNA2 Shader Lifter**: `MIMG` texture sampling, `DS` LDS shared memory + barriers, `wave64` → `wave32` emulation, Mesh/Amplification shader (`MS`/`AS`) lifting, validated via `spirv-val` + `dxc.exe` SM 6.5/6.6 on live GTX 1650 Ti.
- [ ] **Sprint 3 — Live Real-Title Bring-Up**: Automated bring-up suite running official MIT **Microsoft `Xbox-GDK-Samples`** and open-source Gen9 titles with live frame capture, GPU pixel readback, and automated per-title compatibility reports.

---

## Contributing & License

Contributions, bug reports, hardware probe logs, and clean-room PRs are welcome! See [`AGENTS.md`](AGENTS.md) for engineering guidelines and coding standards.

Licensed under the **[MIT License](LICENSE)**.
