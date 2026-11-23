// OpenScarlett Real-Time System Resource & Native Sampling CPU Profiler
// Attaches non-invasively to a running guest process (default: Minecraft.Windows.exe),
// logs 1 Hz system/process/GPU/VRAM/IO telemetry to CSV, and runs a low-overhead
// 200 Hz native stack-walking sampler on the hottest threads to pinpoint exact
// CPU bottlenecks across OpenScarlett shims, D3D12Core, and guest code.
//
// Usage:
//   os_live_profiler.exe [--process Minecraft.Windows.exe | --pid <N>]
//                        [--duration <seconds, 0=until Ctrl+C>]
//                        [--interval-ms <sample period, default 5>]
//                        [--out-dir <path, default E:\OpenScarlett\.tmp\perf_profile>]

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <dbghelp.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "psapi.lib")

namespace fs = std::filesystem;

namespace {

std::atomic<bool> g_stop_requested{false};

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT ||
        ctrl_type == CTRL_CLOSE_EVENT) {
        g_stop_requested.store(true, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}

uint64_t FileTimeToU64(const FILETIME& ft) noexcept {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::string ToLowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

DWORD FindProcessByName(const std::string& target_name) {
    const std::string needle = ToLowerAscii(target_name);
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0u);
    if (snap == INVALID_HANDLE_VALUE) return 0u;

    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(pe);
    DWORD found_pid = 0u;
    if (::Process32First(snap, &pe)) {
        do {
            if (ToLowerAscii(pe.szExeFile) == needle) {
                found_pid = pe.th32ProcessID;
                break;
            }
        } while (::Process32Next(snap, &pe));
    }
    ::CloseHandle(snap);
    return found_pid;
}

struct ModuleRange {
    uint64_t base = 0ull;
    uint64_t end = 0ull;
    std::string name;
};

std::vector<ModuleRange> SnapshotModules(DWORD pid) {
    std::vector<ModuleRange> out;
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return out;
    MODULEENTRY32 me{};
    me.dwSize = sizeof(me);
    if (::Module32First(snap, &me)) {
        do {
            ModuleRange r{};
            r.base = reinterpret_cast<uint64_t>(me.modBaseAddr);
            r.end = r.base + static_cast<uint64_t>(me.modBaseSize);
            r.name = me.szModule;
            out.push_back(std::move(r));
        } while (::Module32Next(snap, &me));
    }
    ::CloseHandle(snap);
    std::sort(out.begin(), out.end(),
              [](const ModuleRange& a, const ModuleRange& b) { return a.base < b.base; });
    return out;
}

const ModuleRange* FindModuleForAddr(const std::vector<ModuleRange>& mods, uint64_t addr) noexcept {
    auto it = std::upper_bound(
        mods.begin(), mods.end(), addr,
        [](uint64_t a, const ModuleRange& m) { return a < m.base; });
    if (it == mods.begin()) return nullptr;
    --it;
    if (addr >= it->base && addr < it->end) {
        return &(*it);
    }
    return nullptr;
}

struct SymbolResolver {
    HANDLE proc = nullptr;
    bool sym_ok = false;
    std::vector<ModuleRange> modules;
    std::unordered_map<uint64_t, std::string> cache;

    void Init(HANDLE hproc, DWORD pid, const std::string& extra_sym_path) {
        proc = hproc;
        modules = SnapshotModules(pid);
        ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        sym_ok = (::SymInitialize(hproc, extra_sym_path.empty() ? nullptr : extra_sym_path.c_str(), TRUE) != FALSE);
    }

    void Shutdown() {
        if (sym_ok && proc) {
            ::SymCleanup(proc);
            sym_ok = false;
        }
    }

    void RefreshModules(DWORD pid) {
        modules = SnapshotModules(pid);
        if (sym_ok && proc) {
            ::SymRefreshModuleList(proc);
        }
    }

    std::string ModuleOf(uint64_t addr) const {
        const ModuleRange* m = FindModuleForAddr(modules, addr);
        return m ? m->name : "<unknown>";
    }

    std::string Resolve(uint64_t addr) {
        auto it = cache.find(addr);
        if (it != cache.end()) return it->second;

        const ModuleRange* m = FindModuleForAddr(modules, addr);
        std::string mod_name = m ? m->name : "<unknown>";
        uint64_t rva = m ? (addr - m->base) : addr;

        std::string result;
        if (sym_ok && proc) {
            alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
            auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
            si->SizeOfStruct = sizeof(SYMBOL_INFO);
            si->MaxNameLen = MAX_SYM_NAME;
            DWORD64 disp = 0;
            if (::SymFromAddr(proc, addr, &disp, si) != FALSE && si->NameLen > 0) {
                char line[512];
                // Only trust symbol name if displacement is reasonable (< 64 KiB) to avoid bogus distant export names.
                if (disp < 0x10000ull) {
                    std::snprintf(line, sizeof(line), "%s!%s+0x%llX (rva +0x%llX)",
                                  mod_name.c_str(), si->Name,
                                  static_cast<unsigned long long>(disp),
                                  static_cast<unsigned long long>(rva));
                    result = line;
                }
            }
        }
        if (result.empty()) {
            char line[256];
            std::snprintf(line, sizeof(line), "%s+0x%llX",
                          mod_name.c_str(), static_cast<unsigned long long>(rva));
            result = line;
        }
        cache.emplace(addr, result);
        return result;
    }
};

struct GpuPdhMonitor {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER gpu_3d_counter = nullptr;
    PDH_HCOUNTER gpu_all_counter = nullptr;
    PDH_HCOUNTER vram_ded_counter = nullptr;
    PDH_HCOUNTER vram_shr_counter = nullptr;
    bool initialized = false;

    void Init(DWORD pid) {
        if (::PdhOpenQueryA(nullptr, 0, &query) != ERROR_SUCCESS) return;
        char path[256];
        std::snprintf(path, sizeof(path),
                      "\\GPU Engine(pid_%lu_*engtype_3D)\\Utilization Percentage",
                      static_cast<unsigned long>(pid));
        ::PdhAddEnglishCounterA(query, path, 0, &gpu_3d_counter);

        std::snprintf(path, sizeof(path),
                      "\\GPU Engine(pid_%lu_*)\\Utilization Percentage",
                      static_cast<unsigned long>(pid));
        ::PdhAddEnglishCounterA(query, path, 0, &gpu_all_counter);

        std::snprintf(path, sizeof(path),
                      "\\GPU Process Memory(pid_%lu_*)\\Dedicated Usage",
                      static_cast<unsigned long>(pid));
        ::PdhAddEnglishCounterA(query, path, 0, &vram_ded_counter);

        std::snprintf(path, sizeof(path),
                      "\\GPU Process Memory(pid_%lu_*)\\Shared Usage",
                      static_cast<unsigned long>(pid));
        ::PdhAddEnglishCounterA(query, path, 0, &vram_shr_counter);

        ::PdhCollectQueryData(query);
        initialized = true;
    }

    static double SumWildcardDouble(PDH_HCOUNTER counter) {
        if (!counter) return 0.0;
        DWORD buf_sz = 0u;
        DWORD item_cnt = 0u;
        PDH_STATUS st = ::PdhGetFormattedCounterArrayA(
            counter, PDH_FMT_DOUBLE, &buf_sz, &item_cnt, nullptr);
        if (st != static_cast<PDH_STATUS>(PDH_MORE_DATA) || buf_sz == 0u) {
            PDH_FMT_COUNTERVALUE v{};
            if (::PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS &&
                v.CStatus == PDH_CSTATUS_VALID_DATA) {
                return v.doubleValue;
            }
            return 0.0;
        }
        std::vector<uint8_t> buf(buf_sz);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_A*>(buf.data());
        if (::PdhGetFormattedCounterArrayA(counter, PDH_FMT_DOUBLE, &buf_sz, &item_cnt, items) != ERROR_SUCCESS) {
            return 0.0;
        }
        double sum = 0.0;
        for (DWORD i = 0; i < item_cnt; ++i) {
            if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA ||
                items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA) {
                sum += items[i].FmtValue.doubleValue;
            }
        }
        return sum;
    }

    void Sample(double& out_gpu_3d_pct, double& out_gpu_all_pct,
                double& out_vram_ded_mb, double& out_vram_shr_mb) {
        out_gpu_3d_pct = 0.0;
        out_gpu_all_pct = 0.0;
        out_vram_ded_mb = 0.0;
        out_vram_shr_mb = 0.0;
        if (!initialized || !query) return;
        if (::PdhCollectQueryData(query) != ERROR_SUCCESS) return;
        out_gpu_3d_pct = SumWildcardDouble(gpu_3d_counter);
        out_gpu_all_pct = SumWildcardDouble(gpu_all_counter);
        out_vram_ded_mb = SumWildcardDouble(vram_ded_counter) / (1024.0 * 1024.0);
        out_vram_shr_mb = SumWildcardDouble(vram_shr_counter) / (1024.0 * 1024.0);
    }

    void Shutdown() {
        if (query) {
            ::PdhCloseQuery(query);
            query = nullptr;
        }
        initialized = false;
    }
};

struct ThreadTrack {
    DWORD tid = 0u;
    HANDLE handle = nullptr;
    uint64_t last_cpu_100ns = 0ull;
    uint64_t total_delta_100ns = 0ull;
    uint64_t recent_delta_100ns = 0ull;
    uint64_t samples_taken = 0ull;
    std::string last_leaf;
};

struct StackSample {
    DWORD tid = 0u;
    std::vector<uint64_t> frames;
};

bool CaptureThreadStack(HANDLE hproc, HANDLE hthread, std::vector<uint64_t>& out_frames) {
    out_frames.clear();
    if (::SuspendThread(hthread) == static_cast<DWORD>(-1)) {
        return false;
    }
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    const BOOL got_ctx = ::GetThreadContext(hthread, &ctx);
    if (!got_ctx) {
        ::ResumeThread(hthread);
        return false;
    }

    // Walk up to 24 frames using StackWalk64 while suspended (fast, <150us).
    // Note: StackWalk64 emits frame 0 (ctx.Rip) on its first call, then unwinds upward.
    STACKFRAME64 sf{};
    sf.AddrPC.Offset = ctx.Rip;
    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rbp;
    sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp;
    sf.AddrStack.Mode = AddrModeFlat;

    out_frames.reserve(24u);
    CONTEXT walk_ctx = ctx;
    for (int depth = 0; depth < 24; ++depth) {
        if (!::StackWalk64(
                IMAGE_FILE_MACHINE_AMD64,
                hproc,
                hthread,
                &sf,
                &walk_ctx,
                nullptr,
                ::SymFunctionTableAccess64,
                ::SymGetModuleBase64,
                nullptr)) {
            break;
        }
        if (sf.AddrPC.Offset == 0ull) {
            break;
        }
        if (!out_frames.empty() && sf.AddrPC.Offset == out_frames.back()) {
            break;
        }
        out_frames.push_back(sf.AddrPC.Offset);
    }

    // Fallback: if StackWalk64 stopped at a syscall leaf, read return address at [RSP].
    if (out_frames.size() <= 1u && ctx.Rsp != 0ull) {
        uint64_t ret_pc = 0ull;
        SIZE_T nread = 0u;
        if (::ReadProcessMemory(hproc, reinterpret_cast<LPCVOID>(ctx.Rsp), &ret_pc, sizeof(ret_pc), &nread) &&
            nread == sizeof(ret_pc) && ret_pc > 0x10000ull) {
            if (out_frames.empty()) out_frames.push_back(ctx.Rip);
            if (ret_pc != out_frames.back()) out_frames.push_back(ret_pc);
        }
    }

    ::ResumeThread(hthread);
    return !out_frames.empty();
}

void RefreshThreadHandles(DWORD pid, std::unordered_map<DWORD, ThreadTrack>& threads) {
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0u);
    if (snap == INVALID_HANDLE_VALUE) return;

    std::unordered_map<DWORD, bool> seen;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (::Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            seen[te.th32ThreadID] = true;
            if (threads.find(te.th32ThreadID) == threads.end()) {
                HANDLE ht = ::OpenThread(
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                    FALSE, te.th32ThreadID);
                if (ht) {
                    ThreadTrack tr{};
                    tr.tid = te.th32ThreadID;
                    tr.handle = ht;
                    FILETIME c{}, e{}, k{}, u{};
                    if (::GetThreadTimes(ht, &c, &e, &k, &u)) {
                        tr.last_cpu_100ns = FileTimeToU64(k) + FileTimeToU64(u);
                    }
                    threads.emplace(te.th32ThreadID, tr);
                }
            }
        } while (::Thread32Next(snap, &te));
    }
    ::CloseHandle(snap);

    for (auto it = threads.begin(); it != threads.end();) {
        if (seen.find(it->first) == seen.end()) {
            if (it->second.handle) ::CloseHandle(it->second.handle);
            it = threads.erase(it);
        } else {
            ++it;
        }
    }
}

std::string MakeTimestampSlug() {
    std::time_t t = std::time(nullptr);
    std::tm tm_now{};
    localtime_s(&tm_now, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d",
                  tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                  tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    std::string proc_name = "Minecraft.Windows.exe";
    DWORD pid = 0u;
    int duration_sec = 20;
    int sample_interval_ms = 5;
    std::string out_dir = "E:\\OpenScarlett\\.tmp\\perf_profile";
    std::string sym_dir = "E:\\OpenScarlett\\.worktrees\\mine-integ\\build";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--process" && i + 1 < argc) {
            proc_name = argv[++i];
        } else if (arg == "--pid" && i + 1 < argc) {
            pid = static_cast<DWORD>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--duration" && i + 1 < argc) {
            duration_sec = std::atoi(argv[++i]);
        } else if (arg == "--interval-ms" && i + 1 < argc) {
            sample_interval_ms = std::max(2, std::atoi(argv[++i]));
        } else if (arg == "--out-dir" && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (arg == "--sym-dir" && i + 1 < argc) {
            sym_dir = argv[++i];
        }
    }

    ::SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    if (pid == 0u) {
        pid = FindProcessByName(proc_name);
        if (pid == 0u) {
            std::fprintf(stderr, "[os_live_profiler] Process '%s' not found. Start the game first or pass --pid.\n",
                         proc_name.c_str());
            return 1;
        }
    }

    HANDLE hproc = ::OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hproc) {
        std::fprintf(stderr, "[os_live_profiler] OpenProcess(PID=%lu) failed (err=%lu).\n",
                     static_cast<unsigned long>(pid), static_cast<unsigned long>(::GetLastError()));
        return 1;
    }

    std::error_code ec;
    fs::create_directories(out_dir, ec);
    const std::string slug = MakeTimestampSlug();
    const fs::path csv_path = fs::path(out_dir) / ("resources_" + slug + ".csv");
    const fs::path report_path = fs::path(out_dir) / ("hotspots_" + slug + ".txt");
    const fs::path latest_csv = fs::path(out_dir) / "latest_resources.csv";
    const fs::path latest_report = fs::path(out_dir) / "latest_hotspots.txt";

    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    const uint32_t num_cpus = std::max<uint32_t>(1u, si.dwNumberOfProcessors);

    SymbolResolver sym;
    sym.Init(hproc, pid, sym_dir);

    GpuPdhMonitor gpu_mon;
    gpu_mon.Init(pid);

    std::unordered_map<DWORD, ThreadTrack> threads;
    RefreshThreadHandles(pid, threads);

    std::ofstream csv(csv_path, std::ios::binary | std::ios::trunc);
    csv << "sec,sys_cpu_pct,sys_ram_pct,proc_cpu_pct,proc_cores_pct,ram_ws_mb,ram_priv_mb,"
           "gpu_3d_pct,gpu_all_pct,vram_ded_mb,vram_shr_mb,io_read_ops_s,io_write_ops_s,"
           "io_read_kb_s,io_write_kb_s,thread_count,top_tid,top_tid_cpu_pct,top_leaf,top_host_procs\n";

    std::printf("=== OpenScarlett Live Profiler ===\n");
    std::printf("Target PID    : %lu (%s)\n", static_cast<unsigned long>(pid), proc_name.c_str());
    std::printf("Logical CPUs  : %u | Sample period: %d ms | Duration: %s\n",
                num_cpus, sample_interval_ms,
                duration_sec > 0 ? (std::to_string(duration_sec) + "s").c_str() : "continuous (Ctrl+C to stop)");
    std::printf("CSV Log       : %s\n", csv_path.string().c_str());
    std::printf("Hotspot Report: %s\n\n", report_path.string().c_str());
    std::fflush(stdout);

    // Baseline CPU & I/O counters
    FILETIME sys_idle0{}, sys_kern0{}, sys_user0{};
    ::GetSystemTimes(&sys_idle0, &sys_kern0, &sys_user0);
    FILETIME p_cre0{}, p_ex0{}, p_kern0{}, p_user0{};
    ::GetProcessTimes(hproc, &p_cre0, &p_ex0, &p_kern0, &p_user0);
    IO_COUNTERS io0{};
    ::GetProcessIoCounters(hproc, &io0);

    uint64_t prev_sys_idle = FileTimeToU64(sys_idle0);
    uint64_t prev_sys_tot = FileTimeToU64(sys_kern0) + FileTimeToU64(sys_user0);
    uint64_t prev_proc_cpu = FileTimeToU64(p_kern0) + FileTimeToU64(p_user0);

    std::unordered_map<uint64_t, uint64_t> leaf_counts;
    std::unordered_map<uint64_t, uint64_t> inclusive_counts;
    std::unordered_map<std::string, uint64_t> module_leaf_counts;
    std::unordered_map<std::string, uint64_t> module_incl_counts;
    std::map<std::vector<uint64_t>, uint64_t> stack_counts;
    std::unordered_map<DWORD, std::unordered_map<uint64_t, uint64_t>> thread_leaf_counts;
    std::unordered_map<DWORD, std::pair<std::string, uint64_t>> prev_host_proc_cpu;
    std::unordered_map<std::string, uint64_t> total_host_proc_cpu;
    uint64_t total_samples = 0ull;

    auto sample_host_procs = [&](double dt_s, std::string& out_top3) {
        out_top3.clear();
        HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0u);
        if (snap == INVALID_HANDLE_VALUE) return;
        std::unordered_map<DWORD, std::pair<std::string, uint64_t>> cur_map;
        std::unordered_map<std::string, uint64_t> delta_by_name;
        PROCESSENTRY32 pe{};
        pe.dwSize = sizeof(pe);
        if (::Process32First(snap, &pe)) {
            do {
                if (pe.th32ProcessID == 0u) continue;
                HANDLE hp = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                if (!hp) continue;
                FILETIME c{}, e{}, k{}, u{};
                if (::GetProcessTimes(hp, &c, &e, &k, &u)) {
                    const uint64_t tot = FileTimeToU64(k) + FileTimeToU64(u);
                    cur_map[pe.th32ProcessID] = {pe.szExeFile, tot};
                    auto pit = prev_host_proc_cpu.find(pe.th32ProcessID);
                    if (pit != prev_host_proc_cpu.end() && tot >= pit->second.second) {
                        const uint64_t d = tot - pit->second.second;
                        if (d > 0ull) {
                            delta_by_name[pe.szExeFile] += d;
                            total_host_proc_cpu[pe.szExeFile] += d;
                        }
                    }
                }
                ::CloseHandle(hp);
            } while (::Process32Next(snap, &pe));
        }
        ::CloseHandle(snap);
        prev_host_proc_cpu.swap(cur_map);

        if (dt_s <= 0.0 || delta_by_name.empty()) return;
        std::vector<std::pair<std::string, uint64_t>> ranked(delta_by_name.begin(), delta_by_name.end());
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        std::ostringstream oss;
        for (size_t i = 0; i < std::min<size_t>(ranked.size(), 3u); ++i) {
            const double pct = 100.0 * (static_cast<double>(ranked[i].second) / 1e7) / (dt_s * num_cpus);
            if (i > 0) oss << "; ";
            oss << ranked[i].first << "=" << std::fixed << std::setprecision(1) << pct << "%";
        }
        out_top3 = oss.str();
    };
    {
        std::string ignored;
        sample_host_procs(0.0, ignored);  // seed baseline
    }

    const auto start_tp = std::chrono::steady_clock::now();
    auto last_sec_tp = start_tp;
    int elapsed_sec = 0;

    while (!g_stop_requested.load(std::memory_order_acquire)) {
        DWORD exit_code = 0u;
        if (!::GetExitCodeProcess(hproc, &exit_code) || exit_code != STILL_ACTIVE) {
            std::printf("[os_live_profiler] Target process exited.\n");
            break;
        }

        const auto now_tp = std::chrono::steady_clock::now();
        if (duration_sec > 0 &&
            std::chrono::duration_cast<std::chrono::seconds>(now_tp - start_tp).count() >= duration_sec) {
            break;
        }

        // Update thread deltas & pick currently running / hottest threads to sample.
        std::vector<ThreadTrack*> active_threads;
        active_threads.reserve(threads.size());
        for (auto& kv : threads) {
            FILETIME c{}, e{}, k{}, u{};
            if (::GetThreadTimes(kv.second.handle, &c, &e, &k, &u)) {
                const uint64_t cur = FileTimeToU64(k) + FileTimeToU64(u);
                const uint64_t delta = (cur >= kv.second.last_cpu_100ns) ? (cur - kv.second.last_cpu_100ns) : 0ull;
                kv.second.last_cpu_100ns = cur;
                kv.second.total_delta_100ns += delta;
                kv.second.recent_delta_100ns += delta;
                if (delta > 0ull) {
                    active_threads.push_back(&kv.second);
                }
            }
        }
        std::sort(active_threads.begin(), active_threads.end(),
                  [](const ThreadTrack* a, const ThreadTrack* b) {
                      return a->recent_delta_100ns > b->recent_delta_100ns;
                  });
        if (active_threads.size() > 6u) {
            active_threads.resize(6u);
        }

        // Sample stacks of active threads (threads that actually ran since last tick).
        std::vector<uint64_t> frames;
        for (ThreadTrack* tr : active_threads) {
            if (CaptureThreadStack(hproc, tr->handle, frames)) {
                ++total_samples;
                ++tr->samples_taken;
                const uint64_t leaf = frames[0];
                ++leaf_counts[leaf];
                ++thread_leaf_counts[tr->tid][leaf];
                ++module_leaf_counts[sym.ModuleOf(leaf)];
                tr->last_leaf = sym.Resolve(leaf);

                // Deduplicate modules & addresses per stack for inclusive counts.
                std::vector<uint64_t> unique_addrs;
                std::vector<std::string> unique_mods;
                for (uint64_t pc : frames) {
                    if (std::find(unique_addrs.begin(), unique_addrs.end(), pc) == unique_addrs.end()) {
                        unique_addrs.push_back(pc);
                        ++inclusive_counts[pc];
                    }
                    std::string m = sym.ModuleOf(pc);
                    if (std::find(unique_mods.begin(), unique_mods.end(), m) == unique_mods.end()) {
                        unique_mods.push_back(m);
                        ++module_incl_counts[m];
                    }
                }
                if (frames.size() > 10u) frames.resize(10u);
                ++stack_counts[frames];
            }
        }

        // 1 Hz system resource snapshot & console HUD update.
        const double dt_sec = std::chrono::duration<double>(now_tp - last_sec_tp).count();
        if (dt_sec >= 1.0) {
            last_sec_tp = now_tp;
            ++elapsed_sec;

            FILETIME si1{}, sk1{}, su1{};
            ::GetSystemTimes(&si1, &sk1, &su1);
            const uint64_t cur_sys_idle = FileTimeToU64(si1);
            const uint64_t cur_sys_tot = FileTimeToU64(sk1) + FileTimeToU64(su1);
            const uint64_t d_idle = cur_sys_idle - prev_sys_idle;
            const uint64_t d_sys = cur_sys_tot - prev_sys_tot;
            prev_sys_idle = cur_sys_idle;
            prev_sys_tot = cur_sys_tot;
            const double sys_cpu_pct = (d_sys > 0ull) ? (100.0 * (1.0 - static_cast<double>(d_idle) / static_cast<double>(d_sys))) : 0.0;

            FILETIME pc1{}, pe1{}, pk1{}, pu1{};
            ::GetProcessTimes(hproc, &pc1, &pe1, &pk1, &pu1);
            const uint64_t cur_proc_cpu = FileTimeToU64(pk1) + FileTimeToU64(pu1);
            const uint64_t d_proc = cur_proc_cpu - prev_proc_cpu;
            prev_proc_cpu = cur_proc_cpu;
            const double proc_cores_pct = (dt_sec > 0.0) ? (100.0 * (static_cast<double>(d_proc) / 1e7) / dt_sec) : 0.0;
            const double proc_cpu_pct = proc_cores_pct / static_cast<double>(num_cpus);

            PROCESS_MEMORY_COUNTERS_EX pmc{};
            pmc.cb = sizeof(pmc);
            ::GetProcessMemoryInfo(hproc, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
            const double ram_ws_mb = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
            const double ram_priv_mb = static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);

            IO_COUNTERS io1{};
            ::GetProcessIoCounters(hproc, &io1);
            const double rd_ops = static_cast<double>(io1.ReadOperationCount - io0.ReadOperationCount) / dt_sec;
            const double wr_ops = static_cast<double>(io1.WriteOperationCount - io0.WriteOperationCount) / dt_sec;
            const double rd_kb = static_cast<double>(io1.ReadTransferCount - io0.ReadTransferCount) / (1024.0 * dt_sec);
            const double wr_kb = static_cast<double>(io1.WriteTransferCount - io0.WriteTransferCount) / (1024.0 * dt_sec);
            io0 = io1;

            double gpu_3d_pct = 0.0, gpu_all_pct = 0.0, vram_ded_mb = 0.0, vram_shr_mb = 0.0;
            gpu_mon.Sample(gpu_3d_pct, gpu_all_pct, vram_ded_mb, vram_shr_mb);

            DWORD top_tid = 0u;
            uint64_t top_tid_delta = 0ull;
            std::string top_leaf = "-";
            for (auto& kv : threads) {
                if (kv.second.recent_delta_100ns > top_tid_delta) {
                    top_tid_delta = kv.second.recent_delta_100ns;
                    top_tid = kv.first;
                    if (!kv.second.last_leaf.empty()) top_leaf = kv.second.last_leaf;
                }
                kv.second.recent_delta_100ns = 0ull;
            }
            const double top_tid_pct = 100.0 * (static_cast<double>(top_tid_delta) / 1e7) / dt_sec;

            MEMORYSTATUSEX mem_st{};
            mem_st.dwLength = sizeof(mem_st);
            ::GlobalMemoryStatusEx(&mem_st);
            const double sys_ram_pct = static_cast<double>(mem_st.dwMemoryLoad);

            std::string top_host_procs;
            sample_host_procs(dt_sec, top_host_procs);

            csv << elapsed_sec << ","
                << std::fixed << std::setprecision(1)
                << sys_cpu_pct << "," << sys_ram_pct << "," << proc_cpu_pct << "," << proc_cores_pct << ","
                << ram_ws_mb << "," << ram_priv_mb << ","
                << gpu_3d_pct << "," << gpu_all_pct << ","
                << vram_ded_mb << "," << vram_shr_mb << ","
                << rd_ops << "," << wr_ops << "," << rd_kb << "," << wr_kb << ","
                << threads.size() << "," << top_tid << "," << top_tid_pct << ",\""
                << top_leaf << "\",\"" << top_host_procs << "\"\n";
            csv.flush();

            std::printf("[%3ds] SysCPU=%5.1f%%(RAM %2.0f%%) | GameCPU=%4.1f%%(%4.0f%%c) | GPU3D=%4.1f%% | RAM=%4.0fMB | VRAM=%4.0fMB | TopHost=[%s]\n",
                        elapsed_sec, sys_cpu_pct, sys_ram_pct, proc_cpu_pct, proc_cores_pct,
                        gpu_3d_pct, ram_ws_mb, vram_ded_mb, top_host_procs.c_str());
            std::fflush(stdout);

            if ((elapsed_sec % 3) == 0) {
                RefreshThreadHandles(pid, threads);
                sym.RefreshModules(pid);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(sample_interval_ms));
    }

    // Write detailed hotspot report
    std::ostringstream rep;
    rep << "=== OpenScarlett Live Profiler Hotspot Report ===\n";
    rep << "Target PID: " << pid << " (" << proc_name << ")\n";
    rep << "Duration  : " << elapsed_sec << " s\n";
    rep << "Samples   : " << total_samples << "\n\n";

    // 0. Top Host CPU Consumers During Capture
    if (elapsed_sec > 0 && !total_host_proc_cpu.empty()) {
        std::vector<std::pair<std::string, uint64_t>> hp(total_host_proc_cpu.begin(), total_host_proc_cpu.end());
        std::sort(hp.begin(), hp.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        rep << "--- 0. Top Host CPU Processes During Capture (Avg % of all " << num_cpus << " CPUs) ---\n";
        for (size_t i = 0; i < std::min<size_t>(hp.size(), 10u); ++i) {
            const double cpu_s = static_cast<double>(hp[i].second) / 1e7;
            const double pct = 100.0 * cpu_s / (static_cast<double>(elapsed_sec) * num_cpus);
            char line[256];
            std::snprintf(line, sizeof(line), "  %6.2f%%  (%5.2fs)  %s\n", pct, cpu_s, hp[i].first.c_str());
            rep << line;
        }
        rep << "\n";
    }

    // 1. CPU time by DLL (Leaf / Self)
    {
        std::vector<std::pair<std::string, uint64_t>> mods(module_leaf_counts.begin(), module_leaf_counts.end());
        std::sort(mods.begin(), mods.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        rep << "--- 1. Self CPU Samples by Module (Leaf PC) ---\n";
        for (size_t i = 0; i < std::min<size_t>(mods.size(), 20u); ++i) {
            const double pct = total_samples ? (100.0 * mods[i].second / total_samples) : 0.0;
            char line[256];
            std::snprintf(line, sizeof(line), "  %6.2f%%  (%6llu)  %s\n",
                          pct, static_cast<unsigned long long>(mods[i].second), mods[i].first.c_str());
            rep << line;
        }
        rep << "\n";
    }

    // 2. Inclusive CPU time by DLL
    {
        std::vector<std::pair<std::string, uint64_t>> mods(module_incl_counts.begin(), module_incl_counts.end());
        std::sort(mods.begin(), mods.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        rep << "--- 2. Inclusive CPU Samples by Module (Anywhere on Stack) ---\n";
        for (size_t i = 0; i < std::min<size_t>(mods.size(), 20u); ++i) {
            const double pct = total_samples ? (100.0 * mods[i].second / total_samples) : 0.0;
            char line[256];
            std::snprintf(line, sizeof(line), "  %6.2f%%  (%6llu)  %s\n",
                          pct, static_cast<unsigned long long>(mods[i].second), mods[i].first.c_str());
            rep << line;
        }
        rep << "\n";
    }

    // 3. Top Leaf Hotspots
    {
        std::vector<std::pair<uint64_t, uint64_t>> leafs(leaf_counts.begin(), leaf_counts.end());
        std::sort(leafs.begin(), leafs.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        rep << "--- 3. Top 30 Self Hotspots (Leaf PC) ---\n";
        for (size_t i = 0; i < std::min<size_t>(leafs.size(), 30u); ++i) {
            const double pct = total_samples ? (100.0 * leafs[i].second / total_samples) : 0.0;
            char line[512];
            std::snprintf(line, sizeof(line), "  %6.2f%%  (%6llu)  %s\n",
                          pct, static_cast<unsigned long long>(leafs[i].second),
                          sym.Resolve(leafs[i].first).c_str());
            rep << line;
        }
        rep << "\n";
    }

    // 4. Top Inclusive Hotspots
    {
        std::vector<std::pair<uint64_t, uint64_t>> incls(inclusive_counts.begin(), inclusive_counts.end());
        std::sort(incls.begin(), incls.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        rep << "--- 4. Top 30 Inclusive Hotspots (Stack Frames) ---\n";
        for (size_t i = 0; i < std::min<size_t>(incls.size(), 30u); ++i) {
            const double pct = total_samples ? (100.0 * incls[i].second / total_samples) : 0.0;
            char line[512];
            std::snprintf(line, sizeof(line), "  %6.2f%%  (%6llu)  %s\n",
                          pct, static_cast<unsigned long long>(incls[i].second),
                          sym.Resolve(incls[i].first).c_str());
            rep << line;
        }
        rep << "\n";
    }

    // 5. Hottest Threads + their top leaf functions
    {
        std::vector<const ThreadTrack*> tlist;
        for (const auto& kv : threads) tlist.push_back(&kv.second);
        std::sort(tlist.begin(), tlist.end(),
                  [](const ThreadTrack* a, const ThreadTrack* b) {
                      return a->total_delta_100ns > b->total_delta_100ns;
                  });
        rep << "--- 5. Top 12 Hottest Threads ---\n";
        for (size_t i = 0; i < std::min<size_t>(tlist.size(), 12u); ++i) {
            const ThreadTrack* tr = tlist[i];
            const double cpu_s = static_cast<double>(tr->total_delta_100ns) / 1e7;
            const double pct = (elapsed_sec > 0) ? (100.0 * cpu_s / static_cast<double>(elapsed_sec)) : 0.0;
            char hdr[256];
            std::snprintf(hdr, sizeof(hdr), "  TID %6lu : %6.2fs CPU (%5.1f%% of 1 core, %llu samples)\n",
                          static_cast<unsigned long>(tr->tid), cpu_s, pct,
                          static_cast<unsigned long long>(tr->samples_taken));
            rep << hdr;
            auto it = thread_leaf_counts.find(tr->tid);
            if (it != thread_leaf_counts.end()) {
                std::vector<std::pair<uint64_t, uint64_t>> tl(it->second.begin(), it->second.end());
                std::sort(tl.begin(), tl.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
                for (size_t j = 0; j < std::min<size_t>(tl.size(), 4u); ++j) {
                    const double tpct = tr->samples_taken ? (100.0 * tl[j].second / tr->samples_taken) : 0.0;
                    char fl[512];
                    std::snprintf(fl, sizeof(fl), "      %5.1f%%  %s\n",
                                  tpct, sym.Resolve(tl[j].first).c_str());
                    rep << fl;
                }
            }
        }
        rep << "\n";
    }

    // 6. Top 15 Full Call Stacks
    {
        std::vector<std::pair<std::vector<uint64_t>, uint64_t>> stacks(stack_counts.begin(), stack_counts.end());
        std::sort(stacks.begin(), stacks.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        rep << "--- 6. Top 15 Sampled Call Stacks ---\n";
        for (size_t i = 0; i < std::min<size_t>(stacks.size(), 15u); ++i) {
            const double pct = total_samples ? (100.0 * stacks[i].second / total_samples) : 0.0;
            char hdr[128];
            std::snprintf(hdr, sizeof(hdr), "  Stack #%zu : %5.2f%% (%llu samples)\n",
                          i + 1u, pct, static_cast<unsigned long long>(stacks[i].second));
            rep << hdr;
            for (uint64_t pc : stacks[i].first) {
                rep << "      " << sym.Resolve(pc) << "\n";
            }
        }
    }

    const std::string report_str = rep.str();
    {
        std::ofstream rf(report_path, std::ios::binary | std::ios::trunc);
        rf << report_str;
    }
    csv.close();
    fs::copy_file(csv_path, latest_csv, fs::copy_options::overwrite_existing, ec);
    fs::copy_file(report_path, latest_report, fs::copy_options::overwrite_existing, ec);

    std::printf("\n%s\nSaved CSV   : %s\nSaved Report: %s\n",
                report_str.c_str(), latest_csv.string().c_str(), latest_report.string().c_str());

    for (auto& kv : threads) {
        if (kv.second.handle) ::CloseHandle(kv.second.handle);
    }
    gpu_mon.Shutdown();
    sym.Shutdown();
    ::CloseHandle(hproc);
    return 0;
}
