# OpenScarlett — Workspace Engineering Rules (`AGENTS.md`)

## 1. Mission & Engineering Philosophy
**OpenScarlett** is an MIT-licensed C++20 compatibility & translation layer for running **Xbox Series X|S (Scarlett / Gen9 Game Core)** x86-64 Windows PE software natively on **Windows 10 & Windows 11 PCs**.

- **Real Binaries & Live GPU Over Guesswork:** Every subsystem must be validated against real compiled workloads — Microsoft's official MIT `Xbox-GDK-Samples`, real D3D12/Mesh Shader binaries via our suspended-process APC hook loader + live COM vtable/import telemetry, and real RDNA2 shader lifting validated by `spirv-val` + `dxc.exe` SM 6.5/6.6 on live D3D12 & Vulkan GPUs.
- **Radical Honesty in Docs:** `README.md` and `docs/status.svg` must always distinguish between:
  - **Live-Tested** (verified on real GPU / real GDK binaries)
  - **Unit-Tested** (verified on synthetic tests)
  - **In Progress / Planned**

## 2. Host Environment & Disk Constraints
- **Host OS:** Windows 10 x64 laptop (8 logical CPU threads)
- **Host GPU:** NVIDIA GeForce GTX 1650 Ti (D3D12 + Vulkan 1.4.329; Turing TU117 — wave32 native, no HW RT cores; always test both hardware GPU and D3D12 WARP!)
- **Toolchain (ALL builds, caches & temp files strictly on `E:\` — NEVER write project/build/temp/cache files to `C:\`!):**
  - Compiler: Visual Studio 2022 Community MSVC (`vcvars64.bat`, C++20)
  - Compiler Cache: `E:\ScarlettX-dev\tools\sccache\sccache.exe` (shared cache at `E:\OpenScarlett\.sccache`, 10 GiB limit, auto-enabled by `CMakeLists.txt`, `build.cmd`, and `build_wt.cmd`)
  - CMake: `E:\ScarlettX-dev\tools\cmake\cmake-4.5.1-windows-x86_64\bin\cmake.exe`
  - Ninja: `E:\ScarlettX-dev\tools\ninja\ninja.exe`
  - DXIL Shader Compiler: `C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe`
  - Rust (optional helper tools): `rustc 1.95.0`
  - Build command: `cmd /c E:\OpenScarlett\build.cmd`
  - Worktree builds for parallel Neon subagents: `cmd /c E:\OpenScarlett\build_wt.cmd <worktree_dir>`

## 3. Fast Build & Test Speed Rules (Mandatory for All Agents)
- **Always use the shared `sccache` cache** (`build.cmd` / `build_wt.cmd` wire `SCCACHE_DIR=E:\OpenScarlett\.sccache` automatically; inspect hit rates with `cmd /c E:\OpenScarlett\build.cmd --cache-stats`). Never set `OPENSCARLETT_NO_CACHE=1` unless debugging a compiler-cache issue.
- **Fast inner-loop iteration (never rebuild/run all 75+ tests on every edit):**
  - Build specific targets only while iterating:
    - Main tree: `cmd /c E:\OpenScarlett\build.cmd --build-only <target1> [target2...]`
    - Worktree: `cmd /c E:\OpenScarlett\build_wt.cmd <wt_dir> --build-only <target1> [target2...]`
  - Run only the relevant test subset while iterating:
    - Main tree: `cmd /c E:\OpenScarlett\build.cmd --test <ctest_regex>`
    - Worktree: `cmd /c E:\OpenScarlett\build_wt.cmd <wt_dir> --test <ctest_regex>`
  - Reserve the full `build.cmd` / `build_wt.cmd` (all targets + full parallel CTest) for final pre-commit / pre-push verification.
- **Parallel CTest & `RESOURCE_LOCK` hygiene:**
  - `build.cmd` and `build_wt.cmd` run `ctest --parallel 4` (overridable via `OPENSCARLETT_CTEST_JOBS`) with critical-path `COST` scheduling and `RESOURCE_LOCK` / `RUN_SERIAL` guards in `cmake/build_speedups.cmake`.
  - When adding a new test:
    - If it opens a Win32 swapchain/HWND or checks window focus, register it under `RESOURCE_LOCK window` in `cmake/build_speedups.cmake`.
    - If it touches live host WASAPI audio, register it under `RESOURCE_LOCK audio` (or set `OPENSCARLETT_AUDIO_NULL=1`).
    - If it shares `OPENSCARLETT_SAVE_ROOT` / VFS overlays, register it under `RESOURCE_LOCK vfs_saves`.
    - If it enforces a strict wall-clock benchmark threshold, mark it `RUN_SERIAL TRUE`.
- **Concurrency throttling for parallel subagents:**
  - When 3–4 parallel worktree agents build or test simultaneously on this 8-thread host, cap per-worker parallelism via environment variables (`OPENSCARLETT_BUILD_JOBS=3`, `OPENSCARLETT_CTEST_JOBS=2`) so workers do not thrash CPU/RAM/GPU while still sharing warm `.sccache` hits.
  - For a single active build/integration pass, leave the fast defaults (`build.cmd`: all cores / `ctest -j4`; `build_wt.cmd`: 6 build jobs / `ctest -j4`).
- **One-time host OS tip (user admin action):** Excluding `E:\OpenScarlett` and `E:\ScarlettX-dev` from Windows Defender real-time scanning (`Add-MpPreference -ExclusionPath 'E:\OpenScarlett','E:\ScarlettX-dev'`) avoids scanning hundreds of `.obj`/`.dll`/`.exe` files per build.

## 4. Commit-Often Visibility & Parallel Neon Workflow
- **Commit & push frequently** in small, atomic, well-titled conventional commits (`feat(...)`, `fix(...)`, `test(...)`, `docs(...)`) so GitHub shows clear, transparent real-time progress.
- **Regenerate status badges** (`python tools/generate_status.py --build-dir build`) after every milestone so `README.md` stays accurate and readable for humans.
- **Parallel Neon Subagents:** When work splits across independent subsystems, create isolated git worktrees under `E:\OpenScarlett\.worktrees\<name>` (never `C:\`), assign non-overlapping files to parallel `neon` subagents, require 0 warnings + 100% CTest pass per worktree, then merge cleanly into `main`, re-run full CTest + `generate_status.py`, and push.
