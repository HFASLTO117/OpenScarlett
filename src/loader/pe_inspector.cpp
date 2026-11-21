#include "openscarlett/loader/pe_inspector.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

#include "openscarlett/loader/vfs_mounts.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace openscarlett {

namespace {

template <typename T>
bool ReadPod(const std::vector<uint8_t>& buf, size_t offset, T& out) {
    if (offset > buf.size() || sizeof(T) > buf.size() - offset) return false;
    std::memcpy(&out, buf.data() + offset, sizeof(T));
    return true;
}

bool ReadCString(const std::vector<uint8_t>& buf, size_t offset, size_t max_len, std::string& out) {
    if (offset >= buf.size()) return false;
    size_t limit = std::min(buf.size() - offset, max_len);
    size_t len = 0;
    while (len < limit && buf[offset + len] != 0) ++len;
    if (len == limit && (offset + len >= buf.size() || buf[offset + len] != 0)) return false;
    out.assign(reinterpret_cast<const char*>(buf.data() + offset), len);
    return true;
}

std::string ToLower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string TrimAscii(std::string_view s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])) != 0) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) != 0) --e;
    return std::string(s.substr(b, e - b));
}

struct SectionRange {
    uint32_t vaddr = 0;
    uint32_t vsize = 0;
    uint32_t raw_ptr = 0;
    uint32_t raw_size = 0;
};

bool RvaToFileOffset(const std::vector<SectionRange>& sections, uint32_t rva, size_t file_size, size_t& out_off) {
    for (const auto& s : sections) {
        uint32_t span = std::max(s.vsize, s.raw_size);
        if (rva >= s.vaddr && rva - s.vaddr < span) {
            uint32_t delta = rva - s.vaddr;
            if (delta >= s.raw_size) return false;
            size_t off = static_cast<size_t>(s.raw_ptr) + delta;
            if (off >= file_size) return false;
            out_off = off;
            return true;
        }
    }
    return false;
}

bool FileExistsInDirW(std::wstring_view dir, std::wstring_view filename, std::wstring& out_full) {
    if (dir.empty() || filename.empty()) {
        return false;
    }
    std::wstring candidate(dir);
    while (candidate.size() > 3 && (candidate.back() == L'\\' || candidate.back() == L'/')) {
        candidate.pop_back();
    }
    if (!candidate.empty() && candidate.back() != L'\\') {
        candidate.push_back(L'\\');
    }
    candidate.append(filename);
#if defined(_WIN32)
    DWORD attrs = ::GetFileAttributesW(candidate.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        out_full = std::move(candidate);
        return true;
    }
    return false;
#else
    std::ifstream ifs(WideToUtf8(candidate));
    if (ifs.good()) {
        out_full = std::move(candidate);
        return true;
    }
    return false;
#endif
}

std::string ReadEnvVarA(const char* name) {
#if defined(_WIN32)
    DWORD needed = ::GetEnvironmentVariableA(name, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::string out(needed, '\0');
    DWORD written = ::GetEnvironmentVariableA(name, out.data(), needed);
    if (written == 0 || written >= needed) {
        return {};
    }
    out.resize(written);
    return out;
#else
    (void)name;
    return {};
#endif
}

bool ParsePolicyToken(std::string_view token, DllRoutePolicy& out_policy) noexcept {
    const std::string lo = ToLower(TrimAscii(token));
    if (lo == "shim") {
        out_policy = DllRoutePolicy::Shim;
        return true;
    }
    if (lo == "guest" || lo == "native" || lo == "nativeguest" || lo == "nativeguestdll") {
        out_policy = DllRoutePolicy::NativeGuestDll;
        return true;
    }
    if (lo == "windows" || lo == "win" || lo == "system" || lo == "os") {
        out_policy = DllRoutePolicy::Windows;
        return true;
    }
    return false;
}

#if defined(_WIN32)
bool ReadMappedPeHeaders64(const void* module_base,
                           const uint8_t*& out_base,
                           const IMAGE_NT_HEADERS64*& out_nt,
                           uint32_t& out_image_size) noexcept {
    if (!module_base) {
        return false;
    }
    const auto* base = static_cast<const uint8_t*>(module_base);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return false;
    }
    out_base = base;
    out_nt = nt;
    out_image_size = nt->OptionalHeader.SizeOfImage;
    return out_image_size >= static_cast<uint32_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64);
}

bool MappedVaToRva(uint64_t va,
                   uintptr_t actual_base,
                   uint64_t preferred_base,
                   uint32_t image_size,
                   uint32_t& out_rva) noexcept {
    if (va >= static_cast<uint64_t>(actual_base) &&
        va - static_cast<uint64_t>(actual_base) < image_size) {
        out_rva = static_cast<uint32_t>(va - static_cast<uint64_t>(actual_base));
        return true;
    }
    if (va >= preferred_base && va - preferred_base < image_size) {
        out_rva = static_cast<uint32_t>(va - preferred_base);
        return true;
    }
    return false;
}
#endif

}  // namespace

const char* DllCategoryName(DllCategory category) noexcept {
    switch (category) {
        case DllCategory::WindowsNative: return "WindowsNative";
        case DllCategory::ConsoleExclusive: return "ConsoleExclusive";
        case DllCategory::GdkRuntime: return "GdkRuntime";
        case DllCategory::Middleware: return "Middleware";
        case DllCategory::Other: return "Other";
    }
    return "Other";
}

const char* DllRoutePolicyName(DllRoutePolicy policy) noexcept {
    switch (policy) {
        case DllRoutePolicy::Shim: return "Shim";
        case DllRoutePolicy::NativeGuestDll: return "NativeGuestDll";
        case DllRoutePolicy::Windows: return "Windows";
    }
    return "Windows";
}

DllCategory ClassifyImportDll(const std::string& dll_name) {
    const std::string lo = ToLower(dll_name);
    if (lo == "d3d12_xs.dll" || lo == "d3d12_x.dll" || lo == "xg_xs.dll" || lo == "xg.dll" ||
        lo == "acphal.dll" || lo == "kernelx.dll" || lo == "xmem.dll" || lo == "dstorage_xs.dll" ||
        lo == "xboxbvhbuilder.dll" || lo == "chad.dll" || lo == "xapu.dll" || lo == "xcrdapi.dll" ||
        lo == "pixevt.dll") {
        return DllCategory::ConsoleExclusive;
    }
    if (lo == "xgameruntime.dll" || lo == "gameinput.dll" || lo == "dstorage.dll" ||
        lo == "xaudio2_9.dll" || lo == "libhttpclient.gdk.dll" || lo == "xcurl.dll" ||
        lo == "microsoft.xbox.services.gdk.c.dll" || lo == "xsapi.dll" || lo == "winrt_xs.dll") {
        return DllCategory::GdkRuntime;
    }
    if (lo.find("bink2") != std::string::npos || lo.find("fidelityfx") != std::string::npos ||
        lo.find("fsr") != std::string::npos || lo.find("wwise") != std::string::npos ||
        lo.find("fmod") != std::string::npos) {
        return DllCategory::Middleware;
    }
    if (lo.rfind("api-ms-win-", 0) == 0 || lo.rfind("ext-ms-win-", 0) == 0 ||
        lo == "kernel32.dll" || lo == "kernelbase.dll" || lo == "ntdll.dll" || lo == "user32.dll" ||
        lo == "gdi32.dll" || lo == "advapi32.dll" || lo == "ole32.dll" || lo == "combase.dll" ||
        lo == "shell32.dll" || lo == "ws2_32.dll" || lo == "bcrypt.dll" || lo == "crypt32.dll" ||
        lo == "ucrtbase.dll" || lo == "vcruntime140.dll" || lo == "vcruntime140_1.dll" ||
        lo == "msvcp140.dll" || lo == "d3d12.dll" || lo == "dxgi.dll" || lo == "rpcrt4.dll" ||
        lo == "winmm.dll" || lo == "dbghelp.dll" || lo == "shlwapi.dll" || lo == "imm32.dll") {
        return DllCategory::WindowsNative;
    }
    return DllCategory::Other;
}

// ---------------------------------------------------------------------------
// Data-Driven Per-DLL Routing Table
// ---------------------------------------------------------------------------
std::string DllRoutingTable::NormalizeCanonicalName(std::string_view raw_name) {
    std::string trimmed = TrimAscii(raw_name);
    if (trimmed.empty()) {
        return {};
    }
    size_t slash = trimmed.find_last_of("\\/");
    if (slash != std::string::npos) {
        trimmed = trimmed.substr(slash + 1);
    }
    if (trimmed.empty()) {
        return {};
    }
    const std::string lo = ToLower(trimmed);
    if (lo.size() < 4 || lo.compare(lo.size() - 4, 4, ".dll") != 0) {
        trimmed.append(".dll");
    }
    return trimmed;
}

std::string DllRoutingTable::NormalizeDllKey(std::string_view raw_name) {
    return ToLower(NormalizeCanonicalName(raw_name));
}

DllRoutingTable::DllRoutingTable() {
    struct BuiltinShimSpec {
        const char* canonical_name;
        int priority;
    };
    // All 19 built-in OpenScarlett shims (10 existing + 9 Wave 1 secondary shims).
    static constexpr BuiltinShimSpec kBuiltinShims[] = {
        // Priority 0: base shims
        {"kernelx.dll", 0},
        {"xgameruntime.dll", 0},
        {"GameInput.dll", 0},
        {"XAudio2_9.dll", 0},
        // Priority 1: core shims
        {"d3d12_xs.dll", 1},
        {"xg_xs.dll", 1},
        {"AcpHal.dll", 1},
        {"dstorage_xs.dll", 1},
        {"winrt_xs.dll", 1},
        {"xmem.dll", 1},
        {"pixevt.dll", 1},
        {"xcrdapi.dll", 1},
        {"libHttpClient.GDK.dll", 1},
        {"XCurl.dll", 1},
        {"Bink2Scarlett.dll", 1},
        {"amd_fidelityfx_upscaler_xbox.dll", 1},
        {"XboxBvhBuilder.dll", 1},
        // Priority 2: dependent shims
        {"Microsoft.Xbox.Services.GDK.C.dll", 2},
        {"amd_fidelityfx_loader_xbox.dll", 2},
    };
    for (const auto& spec : kBuiltinShims) {
        const std::string key = NormalizeDllKey(spec.canonical_name);
        rules_[key] = Entry{
            DllRouteRule{spec.canonical_name, DllRoutePolicy::Shim, spec.priority},
            false,
        };
    }
}

DllRoutingTable DllRoutingTable::CreateWithEnvOverrides() {
    DllRoutingTable table;
    const std::string env_spec = ReadEnvVarA("OPENSCARLETT_DLL_ROUTING");
    if (!env_spec.empty()) {
        table.ApplyOverrides(env_spec);
    }
    return table;
}

void DllRoutingTable::SetRule(std::string_view dll_name, DllRoutePolicy policy, int load_priority) {
    const std::string canonical = NormalizeCanonicalName(dll_name);
    if (canonical.empty()) {
        return;
    }
    const std::string key = ToLower(canonical);
    auto it = rules_.find(key);
    if (it != rules_.end()) {
        it->second.rule.policy = policy;
        if (policy != DllRoutePolicy::Shim) {
            it->second.rule.load_priority = (policy == DllRoutePolicy::NativeGuestDll) ? 5 : 10;
        } else if (load_priority >= 0) {
            it->second.rule.load_priority = load_priority;
        }
        it->second.from_override = true;
    } else {
        const int prio = (policy == DllRoutePolicy::Shim)
                             ? load_priority
                             : ((policy == DllRoutePolicy::NativeGuestDll) ? 5 : 10);
        rules_[key] = Entry{DllRouteRule{canonical, policy, prio}, true};
    }
}

void DllRoutingTable::ApplyOverrides(std::string_view override_spec) {
    size_t pos = 0;
    while (pos < override_spec.size()) {
        size_t next = override_spec.find_first_of(";,", pos);
        if (next == std::string_view::npos) {
            next = override_spec.size();
        }
        std::string_view item = override_spec.substr(pos, next - pos);
        pos = next + 1;

        size_t eq = item.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        std::string_view name_part = item.substr(0, eq);
        std::string_view policy_part = item.substr(eq + 1);
        DllRoutePolicy policy = DllRoutePolicy::Windows;
        if (!ParsePolicyToken(policy_part, policy)) {
            continue;
        }
        const std::string key = NormalizeDllKey(name_part);
        auto it = rules_.find(key);
        const int prio = (it != rules_.end() && policy == DllRoutePolicy::Shim)
                             ? it->second.rule.load_priority
                             : 1;
        SetRule(name_part, policy, prio);
    }
}

const DllRouteRule* DllRoutingTable::FindRule(std::string_view dll_name) const {
    const std::string key = NormalizeDllKey(dll_name);
    auto it = rules_.find(key);
    return (it != rules_.end()) ? &it->second.rule : nullptr;
}

DllRouteDecision DllRoutingTable::ResolveRoute(std::string_view dll_name,
                                               std::wstring_view shim_dir,
                                               std::wstring_view guest_dir) const {
    DllRouteDecision out{};
    const std::string canonical_in = NormalizeCanonicalName(dll_name);
    if (canonical_in.empty()) {
        return out;
    }
    const std::string key = ToLower(canonical_in);
    out.canonical_name = canonical_in;

    auto it = rules_.find(key);
    if (it != rules_.end()) {
        const Entry& entry = it->second;
        out.canonical_name = entry.rule.canonical_name;
        out.load_priority = entry.rule.load_priority;
        out.from_override = entry.from_override;

        if (entry.rule.policy == DllRoutePolicy::Windows) {
            out.policy = DllRoutePolicy::Windows;
            return out;
        }
        if (entry.rule.policy == DllRoutePolicy::NativeGuestDll) {
            out.policy = DllRoutePolicy::NativeGuestDll;
            const std::wstring canon_w = Utf8ToWide(out.canonical_name);
            FileExistsInDirW(guest_dir, canon_w, out.resolved_path);
            return out;
        }

        // entry.rule.policy == DllRoutePolicy::Shim
        const std::wstring canon_w = Utf8ToWide(out.canonical_name);
        if (FileExistsInDirW(shim_dir, canon_w, out.resolved_path)) {
            out.policy = DllRoutePolicy::Shim;
            return out;
        }
        // If shim_dir was provided and shim DLL is not present there, fall back to NativeGuestDll if present.
        if (!shim_dir.empty() && !guest_dir.empty() &&
            FileExistsInDirW(guest_dir, canon_w, out.resolved_path)) {
            out.policy = DllRoutePolicy::NativeGuestDll;
            out.load_priority = 5;
            return out;
        }
        out.policy = DllRoutePolicy::Shim;
        return out;
    }

    // No explicit rule: check if WindowsNative vs present in guest package directory.
    if (ClassifyImportDll(canonical_in) == DllCategory::WindowsNative) {
        out.policy = DllRoutePolicy::Windows;
        out.load_priority = 10;
        return out;
    }

    const std::wstring canon_w = Utf8ToWide(canonical_in);
    if (!guest_dir.empty() && FileExistsInDirW(guest_dir, canon_w, out.resolved_path)) {
        out.policy = DllRoutePolicy::NativeGuestDll;
        out.load_priority = 5;
        return out;
    }

    out.policy = DllRoutePolicy::Windows;
    out.load_priority = 10;
    return out;
}

DllRouteDecision DllRoutingTable::ResolveRouteW(std::wstring_view dll_name,
                                                std::wstring_view shim_dir,
                                                std::wstring_view guest_dir) const {
    return ResolveRoute(WideToUtf8(dll_name), shim_dir, guest_dir);
}

std::vector<DllRouteRule> DllRoutingTable::ListRules() const {
    std::vector<DllRouteRule> out;
    out.reserve(rules_.size());
    for (const auto& kv : rules_) {
        out.push_back(kv.second.rule);
    }
    std::sort(out.begin(), out.end(), [](const DllRouteRule& a, const DllRouteRule& b) {
        if (a.load_priority != b.load_priority) {
            return a.load_priority < b.load_priority;
        }
        return ToLower(a.canonical_name) < ToLower(b.canonical_name);
    });
    return out;
}

// ---------------------------------------------------------------------------
// PE32+ Inspector (Imports, Exports & Forwarders, TLS Callbacks, .pdata SEH)
// ---------------------------------------------------------------------------
Status InspectPeBytes(const std::vector<uint8_t>& bytes,
                      const std::string& label,
                      PeInspectionReport& out_report) {
    out_report = {};
    out_report.file_path = label;

    if (bytes.size() < 0x100) {
        return Status::Error(StatusCode::ParseError, "File too small to be a valid PE image");
    }
    uint16_t mz = 0;
    uint32_t pe_off = 0;
    if (!ReadPod(bytes, 0, mz) || mz != 0x5A4Du || !ReadPod(bytes, 0x3C, pe_off)) {
        return Status::Error(StatusCode::ParseError, "Missing DOS MZ header");
    }
    uint32_t pe_sig = 0;
    if (!ReadPod(bytes, pe_off, pe_sig) || pe_sig != 0x00004550u) {
        return Status::Error(StatusCode::ParseError, "Missing PE\\0\\0 signature");
    }

    const size_t coff_off = static_cast<size_t>(pe_off) + 4;
    uint16_t num_sections = 0;
    uint16_t opt_size = 0;
    uint16_t characteristics = 0;
    if (!ReadPod(bytes, coff_off + 0, out_report.machine) ||
        !ReadPod(bytes, coff_off + 2, num_sections) ||
        !ReadPod(bytes, coff_off + 16, opt_size) ||
        !ReadPod(bytes, coff_off + 18, characteristics)) {
        return Status::Error(StatusCode::ParseError, "Truncated COFF header");
    }
    out_report.section_count = num_sections;
    out_report.is_dll = (characteristics & 0x2000u) != 0;
    out_report.has_relocations = (characteristics & 0x0001u) == 0;

    const size_t opt_off = coff_off + 20;
    uint16_t opt_magic = 0;
    if (opt_size < 0x70 || !ReadPod(bytes, opt_off, opt_magic)) {
        return Status::Error(StatusCode::ParseError, "Truncated PE optional header");
    }
    if (opt_magic != 0x020Bu) {
        return Status::Error(StatusCode::UnsupportedFormat, "Not a PE32+ (64-bit) image");
    }
    out_report.is_pe64 = true;

    uint32_t num_rva_and_sizes = 0;
    if (!ReadPod(bytes, opt_off + 16, out_report.entry_point_rva) ||
        !ReadPod(bytes, opt_off + 24, out_report.image_base) ||
        !ReadPod(bytes, opt_off + 56, out_report.image_size) ||
        !ReadPod(bytes, opt_off + 68, out_report.subsystem) ||
        !ReadPod(bytes, opt_off + 108, num_rva_and_sizes)) {
        return Status::Error(StatusCode::ParseError, "Truncated PE32+ fields");
    }

    const size_t dd_off = opt_off + 112;
    auto read_dir = [&](uint32_t idx, uint32_t& rva, uint32_t& sz) -> bool {
        if (idx >= num_rva_and_sizes) return false;
        return ReadPod(bytes, dd_off + static_cast<size_t>(idx) * 8, rva) &&
               ReadPod(bytes, dd_off + static_cast<size_t>(idx) * 8 + 4, sz);
    };

    const size_t sec_off = opt_off + opt_size;
    std::vector<SectionRange> sections;
    sections.reserve(num_sections);
    for (uint16_t i = 0; i < num_sections; ++i) {
        const size_t base = sec_off + static_cast<size_t>(i) * 40;
        SectionRange sr{};
        if (!ReadPod(bytes, base + 8, sr.vsize) ||
            !ReadPod(bytes, base + 12, sr.vaddr) ||
            !ReadPod(bytes, base + 16, sr.raw_size) ||
            !ReadPod(bytes, base + 20, sr.raw_ptr)) {
            return Status::Error(StatusCode::ParseError, "Truncated section table");
        }
        sections.push_back(sr);
    }

    // 1. Export Directory (index 0) including forwarder strings
    uint32_t exp_rva = 0, exp_sz = 0;
    if (read_dir(0, exp_rva, exp_sz) && exp_rva != 0 && exp_sz >= 40u) {
        out_report.export_dir_rva = exp_rva;
        out_report.export_dir_size = exp_sz;
        size_t exp_off = 0;
        if (RvaToFileOffset(sections, exp_rva, bytes.size(), exp_off)) {
            uint32_t ordinal_base = 0;
            uint32_t num_funcs = 0;
            uint32_t num_names = 0;
            uint32_t addr_funcs_rva = 0;
            uint32_t addr_names_rva = 0;
            uint32_t addr_ords_rva = 0;
            if (ReadPod(bytes, exp_off + 16, ordinal_base) &&
                ReadPod(bytes, exp_off + 20, num_funcs) &&
                ReadPod(bytes, exp_off + 24, num_names) &&
                ReadPod(bytes, exp_off + 28, addr_funcs_rva) &&
                ReadPod(bytes, exp_off + 32, addr_names_rva) &&
                ReadPod(bytes, exp_off + 36, addr_ords_rva)) {
                out_report.export_ordinal_base = ordinal_base;
                const uint32_t capped_funcs = std::min<uint32_t>(num_funcs, 65536u);
                const uint32_t capped_names = std::min<uint32_t>(num_names, 65536u);

                std::unordered_map<uint32_t, std::string> index_to_name;
                size_t names_off = 0;
                size_t ords_off = 0;
                if (capped_names > 0 &&
                    RvaToFileOffset(sections, addr_names_rva, bytes.size(), names_off) &&
                    RvaToFileOffset(sections, addr_ords_rva, bytes.size(), ords_off)) {
                    index_to_name.reserve(capped_names);
                    for (uint32_t i = 0; i < capped_names; ++i) {
                        uint32_t name_rva = 0;
                        uint16_t func_idx = 0;
                        if (!ReadPod(bytes, names_off + static_cast<size_t>(i) * 4, name_rva) ||
                            !ReadPod(bytes, ords_off + static_cast<size_t>(i) * 2, func_idx)) {
                            break;
                        }
                        size_t str_off = 0;
                        std::string sym_name;
                        if (RvaToFileOffset(sections, name_rva, bytes.size(), str_off) &&
                            ReadCString(bytes, str_off, 512, sym_name)) {
                            index_to_name.emplace(static_cast<uint32_t>(func_idx), std::move(sym_name));
                        }
                    }
                }

                size_t funcs_off = 0;
                if (capped_funcs > 0 &&
                    RvaToFileOffset(sections, addr_funcs_rva, bytes.size(), funcs_off)) {
                    out_report.exports.reserve(capped_funcs);
                    for (uint32_t i = 0; i < capped_funcs; ++i) {
                        uint32_t fn_rva = 0;
                        if (!ReadPod(bytes, funcs_off + static_cast<size_t>(i) * 4, fn_rva)) {
                            break;
                        }
                        if (fn_rva == 0) {
                            continue;
                        }
                        PeExportSymbol sym{};
                        sym.ordinal = ordinal_base + i;
                        sym.rva = fn_rva;
                        auto name_it = index_to_name.find(i);
                        if (name_it != index_to_name.end()) {
                            sym.name = name_it->second;
                        }
                        if (fn_rva >= exp_rva && fn_rva < exp_rva + exp_sz) {
                            sym.is_forwarded = true;
                            size_t fwd_off = 0;
                            if (RvaToFileOffset(sections, fn_rva, bytes.size(), fwd_off)) {
                                const size_t max_fwd =
                                    std::min<size_t>(512u, static_cast<size_t>(exp_rva + exp_sz - fn_rva));
                                ReadCString(bytes, fwd_off, max_fwd, sym.forwarder);
                            }
                        }
                        out_report.exports.push_back(std::move(sym));
                    }
                }
            }
        }
    }

    // 2. Exception Directory / x64 .pdata (index 3)
    uint32_t exc_rva = 0, exc_sz = 0;
    if (read_dir(3, exc_rva, exc_sz) && exc_rva != 0 && exc_sz != 0) {
        out_report.exception_dir_rva = exc_rva;
        out_report.exception_dir_size = exc_sz;
        out_report.exception_entry_count = exc_sz / 12u;  // sizeof(RUNTIME_FUNCTION) == 12 on x64
    }

    // 3. TLS Directory & Callback RVAs (index 9)
    uint32_t tls_rva = 0, tls_sz = 0;
    if (read_dir(9, tls_rva, tls_sz) && tls_rva != 0 && tls_sz != 0) {
        out_report.has_tls = true;
        out_report.tls_dir_rva = tls_rva;
        out_report.tls_dir_size = tls_sz;

        size_t tls_off = 0;
        uint64_t addr_of_callbacks_va = 0;
        if (RvaToFileOffset(sections, tls_rva, bytes.size(), tls_off) &&
            ReadPod(bytes, tls_off + 24, addr_of_callbacks_va) &&
            addr_of_callbacks_va >= out_report.image_base) {
            const uint64_t cb_rva_64 = addr_of_callbacks_va - out_report.image_base;
            if (cb_rva_64 < out_report.image_size) {
                size_t cb_off = 0;
                if (RvaToFileOffset(sections, static_cast<uint32_t>(cb_rva_64), bytes.size(), cb_off)) {
                    for (size_t i = 0; i < 256; ++i) {
                        uint64_t fn_va = 0;
                        if (!ReadPod(bytes, cb_off + i * 8, fn_va) || fn_va == 0) {
                            break;
                        }
                        if (fn_va >= out_report.image_base &&
                            fn_va - out_report.image_base < out_report.image_size) {
                            out_report.tls_callback_rvas.push_back(
                                static_cast<uint32_t>(fn_va - out_report.image_base));
                        }
                    }
                }
            }
        }
    }

    // 4. Import & Delay-Load Directories (index 1 & 13)
    std::unordered_map<std::string, size_t> mod_index;
    auto add_module = [&](const std::string& dll_name) -> PeImportModule& {
        std::string lo = ToLower(dll_name);
        auto it = mod_index.find(lo);
        if (it != mod_index.end()) return out_report.imports[it->second];
        PeImportModule m{};
        m.dll_name = dll_name;
        m.category = ClassifyImportDll(dll_name);
        mod_index[lo] = out_report.imports.size();
        out_report.imports.push_back(std::move(m));
        return out_report.imports.back();
    };

    auto parse_thunk_list = [&](uint32_t thunk_rva, bool delay_loaded, PeImportModule& mod) {
        size_t off = 0;
        if (!RvaToFileOffset(sections, thunk_rva, bytes.size(), off)) return;
        for (size_t guard = 0; guard < 16384; ++guard) {
            uint64_t val = 0;
            if (!ReadPod(bytes, off + guard * 8, val) || val == 0) break;
            PeImportSymbol sym{};
            sym.delay_loaded = delay_loaded;
            if ((val & (1ull << 63)) != 0) {
                sym.by_ordinal = true;
                sym.ordinal = static_cast<uint16_t>(val & 0xFFFFu);
            } else {
                size_t hint_off = 0;
                if (RvaToFileOffset(sections, static_cast<uint32_t>(val & 0x7FFFFFFFu), bytes.size(), hint_off)) {
                    ReadPod(bytes, hint_off, sym.ordinal);
                    ReadCString(bytes, hint_off + 2, 512, sym.name);
                }
            }
            mod.symbols.push_back(std::move(sym));
            ++out_report.total_imports;
            if (delay_loaded) ++out_report.delay_load_imports;
            if (mod.category == DllCategory::ConsoleExclusive) ++out_report.console_imports;
            if (mod.category == DllCategory::GdkRuntime) ++out_report.gdk_imports;
        }
    };

    uint32_t imp_rva = 0, imp_sz = 0;
    if (read_dir(1, imp_rva, imp_sz) && imp_rva != 0) {
        size_t off = 0;
        if (RvaToFileOffset(sections, imp_rva, bytes.size(), off)) {
            for (size_t idx = 0; idx < 2048; ++idx) {
                uint32_t oft = 0, name_rva = 0, ft = 0;
                if (!ReadPod(bytes, off + idx * 20 + 0, oft) ||
                    !ReadPod(bytes, off + idx * 20 + 12, name_rva) ||
                    !ReadPod(bytes, off + idx * 20 + 16, ft)) {
                    break;
                }
                if (oft == 0 && name_rva == 0 && ft == 0) break;
                size_t name_off = 0;
                std::string dll;
                if (!RvaToFileOffset(sections, name_rva, bytes.size(), name_off) ||
                    !ReadCString(bytes, name_off, 260, dll)) {
                    continue;
                }
                auto& mod = add_module(dll);
                parse_thunk_list(oft != 0 ? oft : ft, false, mod);
            }
        }
    }

    uint32_t dly_rva = 0, dly_sz = 0;
    if (read_dir(13, dly_rva, dly_sz) && dly_rva != 0) {
        size_t off = 0;
        if (RvaToFileOffset(sections, dly_rva, bytes.size(), off)) {
            for (size_t idx = 0; idx < 1024; ++idx) {
                uint32_t attrs = 0, name_rva = 0, int_rva = 0;
                if (!ReadPod(bytes, off + idx * 32 + 0, attrs) ||
                    !ReadPod(bytes, off + idx * 32 + 4, name_rva) ||
                    !ReadPod(bytes, off + idx * 32 + 16, int_rva)) {
                    break;
                }
                if (name_rva == 0 && int_rva == 0) break;
                if ((attrs & 1u) == 0) continue;
                size_t name_off = 0;
                std::string dll;
                if (!RvaToFileOffset(sections, name_rva, bytes.size(), name_off) ||
                    !ReadCString(bytes, name_off, 260, dll)) {
                    continue;
                }
                auto& mod = add_module(dll);
                parse_thunk_list(int_rva, true, mod);
            }
        }
    }

    return Status::Ok();
}

Status InspectPeFile(const std::string& file_path, PeInspectionReport& out_report) {
    std::ifstream ifs(file_path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        return Status::Error(StatusCode::NotFound, "Cannot open PE file: " + file_path);
    }
    const auto sz = ifs.tellg();
    if (sz <= 0) {
        return Status::Error(StatusCode::ParseError, "Empty PE file: " + file_path);
    }
    ifs.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (!ifs.read(reinterpret_cast<char*>(buf.data()), sz)) {
        return Status::Error(StatusCode::IoError, "Failed reading PE file: " + file_path);
    }
    return InspectPeBytes(buf, file_path, out_report);
}

// ---------------------------------------------------------------------------
// Export-Forwarder Parsing & Chain Resolution
// ---------------------------------------------------------------------------
bool ParseExportForwarder(std::string_view forwarder, PeForwarderTarget& out_target) {
    out_target = {};
    const size_t dot = forwarder.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= forwarder.size()) {
        return false;
    }

    std::string dll(forwarder.substr(0, dot));
    std::string_view sym = forwarder.substr(dot + 1);
    if (dll.empty() || sym.empty()) {
        return false;
    }

    const std::string dll_lo = ToLower(dll);
    if (dll_lo.size() < 4 || dll_lo.compare(dll_lo.size() - 4, 4, ".dll") != 0) {
        dll.append(".dll");
    }
    out_target.dll_name = std::move(dll);

    if (sym[0] == '#') {
        if (sym.size() < 2) {
            return false;
        }
        uint32_t ord = 0;
        for (size_t i = 1; i < sym.size(); ++i) {
            if (sym[i] < '0' || sym[i] > '9') {
                return false;
            }
            ord = ord * 10u + static_cast<uint32_t>(sym[i] - '0');
            if (ord > 0xFFFFu) {
                return false;
            }
        }
        out_target.by_ordinal = true;
        out_target.ordinal = static_cast<uint16_t>(ord);
        return true;
    }

    out_target.by_ordinal = false;
    out_target.symbol_name.assign(sym);
    return true;
}

Status ResolveExportForwarderChain(const std::string& start_dll,
                                   const std::string& start_symbol,
                                   uint16_t start_ordinal,
                                   bool by_ordinal,
                                   const PeModuleBytesResolver& resolver,
                                   PeForwarderResolution& out_resolution,
                                   uint32_t max_depth) {
    out_resolution = {};
    if (!resolver) {
        return Status::Error(StatusCode::InvalidArgument, "Module resolver callback is null");
    }
    if (start_dll.empty() || (!by_ordinal && start_symbol.empty())) {
        return Status::Error(StatusCode::InvalidArgument, "Empty start DLL or export symbol");
    }

    std::unordered_map<std::string, PeInspectionReport> parsed_cache;
    std::unordered_set<std::string> visited;

    std::string cur_dll = start_dll;
    std::string cur_sym = start_symbol;
    uint16_t cur_ord = start_ordinal;
    bool cur_by_ord = by_ordinal;

    for (uint32_t depth = 0; depth <= max_depth; ++depth) {
        std::string norm_dll = cur_dll;
        {
            std::string lo = ToLower(norm_dll);
            if (lo.size() < 4 || lo.compare(lo.size() - 4, 4, ".dll") != 0) {
                norm_dll.append(".dll");
            }
        }
        const std::string dll_key = ToLower(norm_dll);
        const std::string sym_label = cur_by_ord ? ("#" + std::to_string(cur_ord)) : cur_sym;
        const std::string visit_key = dll_key + "!" + sym_label;

        if (!visited.insert(visit_key).second) {
            return Status::Error(StatusCode::ParseError,
                                 "Export forwarder cycle detected at " + norm_dll + "!" + sym_label);
        }
        out_resolution.chain.push_back(norm_dll + "!" + sym_label);

        auto cache_it = parsed_cache.find(dll_key);
        if (cache_it == parsed_cache.end()) {
            std::vector<uint8_t> raw_bytes;
            auto load_st = resolver(norm_dll, raw_bytes);
            if (!load_st.ok()) {
                return load_st;
            }
            PeInspectionReport rep;
            auto parse_st = InspectPeBytes(raw_bytes, norm_dll, rep);
            if (!parse_st.ok()) {
                return parse_st;
            }
            cache_it = parsed_cache.emplace(dll_key, std::move(rep)).first;
        }

        const PeInspectionReport& rep = cache_it->second;
        const PeExportSymbol* found = nullptr;
        for (const auto& exp : rep.exports) {
            if (cur_by_ord) {
                if (exp.ordinal == cur_ord) {
                    found = &exp;
                    break;
                }
            } else {
                if (exp.name == cur_sym) {
                    found = &exp;
                    break;
                }
            }
        }
        if (!found) {
            return Status::Error(StatusCode::NotFound,
                                 "Export not found: " + norm_dll + "!" + sym_label);
        }

        if (!found->is_forwarded) {
            out_resolution.final_dll = norm_dll;
            out_resolution.final_symbol = found->name;
            out_resolution.final_ordinal = static_cast<uint16_t>(found->ordinal & 0xFFFFu);
            out_resolution.by_ordinal = cur_by_ord;
            out_resolution.final_rva = found->rva;
            return Status::Ok();
        }

        if (depth == max_depth) {
            return Status::Error(StatusCode::ParseError,
                                 "Export forwarder chain exceeded max depth (" +
                                     std::to_string(max_depth) + ")");
        }

        PeForwarderTarget next{};
        if (!ParseExportForwarder(found->forwarder, next)) {
            return Status::Error(StatusCode::ParseError,
                                 "Malformed export forwarder string: " + found->forwarder);
        }
        cur_dll = std::move(next.dll_name);
        cur_sym = std::move(next.symbol_name);
        cur_ord = next.ordinal;
        cur_by_ord = next.by_ordinal;
    }

    return Status::Error(StatusCode::ParseError, "Export forwarder chain did not terminate");
}

bool LookupMappedPeExportForwarder(const void* module_base,
                                   const char* proc_name_or_ordinal,
                                   PeForwarderTarget& out_target,
                                   std::string* out_raw_forwarder) {
    out_target = {};
    if (out_raw_forwarder) {
        out_raw_forwarder->clear();
    }
#if !defined(_WIN32)
    (void)module_base;
    (void)proc_name_or_ordinal;
    return false;
#else
    if (!module_base || !proc_name_or_ordinal) {
        return false;
    }
    const uint8_t* base = nullptr;
    const IMAGE_NT_HEADERS64* nt = nullptr;
    uint32_t image_size = 0;
    if (!ReadMappedPeHeaders64(module_base, base, nt, image_size)) {
        return false;
    }

    const auto& exp_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (exp_dir.VirtualAddress == 0 || exp_dir.Size < sizeof(IMAGE_EXPORT_DIRECTORY) ||
        exp_dir.VirtualAddress >= image_size ||
        exp_dir.Size > image_size - exp_dir.VirtualAddress) {
        return false;
    }

    const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + exp_dir.VirtualAddress);
    if (exp->NumberOfFunctions == 0 || exp->AddressOfFunctions == 0 ||
        exp->AddressOfFunctions >= image_size ||
        static_cast<uint64_t>(exp->NumberOfFunctions) * sizeof(uint32_t) >
            image_size - exp->AddressOfFunctions) {
        return false;
    }

    const auto* funcs = reinterpret_cast<const uint32_t*>(base + exp->AddressOfFunctions);
    uint32_t func_index = UINT32_MAX;

    if (reinterpret_cast<uintptr_t>(proc_name_or_ordinal) <= 0xFFFFu) {
        const uint32_t ord = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(proc_name_or_ordinal));
        if (ord < exp->Base || ord - exp->Base >= exp->NumberOfFunctions) {
            return false;
        }
        func_index = ord - exp->Base;
    } else {
        if (exp->NumberOfNames == 0 || exp->AddressOfNames == 0 || exp->AddressOfNameOrdinals == 0 ||
            exp->AddressOfNames >= image_size || exp->AddressOfNameOrdinals >= image_size ||
            static_cast<uint64_t>(exp->NumberOfNames) * sizeof(uint32_t) >
                image_size - exp->AddressOfNames ||
            static_cast<uint64_t>(exp->NumberOfNames) * sizeof(uint16_t) >
                image_size - exp->AddressOfNameOrdinals) {
            return false;
        }

        const auto* names = reinterpret_cast<const uint32_t*>(base + exp->AddressOfNames);
        const auto* ords = reinterpret_cast<const uint16_t*>(base + exp->AddressOfNameOrdinals);

        // O(log N) binary search per PE/COFF sorted name table specification.
        uint32_t lo = 0;
        uint32_t hi = exp->NumberOfNames;
        while (lo < hi) {
            const uint32_t mid = lo + (hi - lo) / 2u;
            const uint32_t name_rva = names[mid];
            if (name_rva == 0 || name_rva >= image_size) {
                break;
            }
            const char* cur_name = reinterpret_cast<const char*>(base + name_rva);
            const int cmp = std::strcmp(proc_name_or_ordinal, cur_name);
            if (cmp == 0) {
                func_index = ords[mid];
                break;
            }
            if (cmp < 0) {
                hi = mid;
            } else {
                lo = mid + 1u;
            }
        }

        // Linear fallback for hand-crafted PE images with unsorted export name tables.
        if (func_index == UINT32_MAX) {
            for (uint32_t i = 0; i < exp->NumberOfNames; ++i) {
                const uint32_t name_rva = names[i];
                if (name_rva == 0 || name_rva >= image_size) {
                    continue;
                }
                const char* cur_name = reinterpret_cast<const char*>(base + name_rva);
                if (std::strcmp(proc_name_or_ordinal, cur_name) == 0) {
                    func_index = ords[i];
                    break;
                }
            }
        }
    }

    if (func_index >= exp->NumberOfFunctions) {
        return false;
    }

    const uint32_t fn_rva = funcs[func_index];
    if (fn_rva < exp_dir.VirtualAddress || fn_rva >= exp_dir.VirtualAddress + exp_dir.Size) {
        return false;
    }

    const size_t max_len = static_cast<size_t>(exp_dir.VirtualAddress + exp_dir.Size - fn_rva);
    const char* fwd_ptr = reinterpret_cast<const char*>(base + fn_rva);
    size_t fwd_len = 0;
    while (fwd_len < max_len && fwd_ptr[fwd_len] != '\0') {
        ++fwd_len;
    }
    if (fwd_len == 0 || fwd_len == max_len) {
        return false;
    }

    const std::string_view fwd_view(fwd_ptr, fwd_len);
    if (!ParseExportForwarder(fwd_view, out_target)) {
        return false;
    }
    if (out_raw_forwarder) {
        out_raw_forwarder->assign(fwd_view);
    }
    return true;
#endif
}

// ---------------------------------------------------------------------------
// TLS Callbacks & x64 .pdata SEH Registration Helpers
// ---------------------------------------------------------------------------
bool ReadMappedPeTlsCallbackRvas(const void* module_base,
                                 std::vector<uint32_t>& out_callback_rvas) {
    out_callback_rvas.clear();
#if !defined(_WIN32)
    (void)module_base;
    return false;
#else
    const uint8_t* base = nullptr;
    const IMAGE_NT_HEADERS64* nt = nullptr;
    uint32_t image_size = 0;
    if (!ReadMappedPeHeaders64(module_base, base, nt, image_size)) {
        return false;
    }

    const auto& tls_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (tls_dir.VirtualAddress == 0 || tls_dir.Size < sizeof(IMAGE_TLS_DIRECTORY64) ||
        tls_dir.VirtualAddress >= image_size ||
        sizeof(IMAGE_TLS_DIRECTORY64) > image_size - tls_dir.VirtualAddress) {
        return false;
    }

    const auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(base + tls_dir.VirtualAddress);
    if (tls->AddressOfCallBacks == 0) {
        return true;
    }

    uint32_t cb_array_rva = 0;
    if (!MappedVaToRva(tls->AddressOfCallBacks,
                       reinterpret_cast<uintptr_t>(base),
                       nt->OptionalHeader.ImageBase,
                       image_size,
                       cb_array_rva)) {
        return false;
    }

    for (size_t i = 0; i < 256; ++i) {
        if (cb_array_rva + (i + 1) * sizeof(uint64_t) > image_size) {
            break;
        }
        uint64_t fn_va = 0;
        std::memcpy(&fn_va, base + cb_array_rva + i * sizeof(uint64_t), sizeof(fn_va));
        if (fn_va == 0) {
            break;
        }
        uint32_t fn_rva = 0;
        if (MappedVaToRva(fn_va,
                          reinterpret_cast<uintptr_t>(base),
                          nt->OptionalHeader.ImageBase,
                          image_size,
                          fn_rva)) {
            out_callback_rvas.push_back(fn_rva);
        }
    }
    return true;
#endif
}

uint32_t InvokePeTlsCallbacks(void* module_base, uint32_t reason, void* reserved) {
#if !defined(_WIN32)
    (void)module_base;
    (void)reason;
    (void)reserved;
    return 0;
#else
    if (!module_base) {
        return 0;
    }
    const uint8_t* base = nullptr;
    const IMAGE_NT_HEADERS64* nt = nullptr;
    uint32_t image_size = 0;
    if (!ReadMappedPeHeaders64(module_base, base, nt, image_size)) {
        return 0;
    }

    const auto& tls_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (tls_dir.VirtualAddress == 0 || tls_dir.Size < sizeof(IMAGE_TLS_DIRECTORY64) ||
        tls_dir.VirtualAddress >= image_size ||
        sizeof(IMAGE_TLS_DIRECTORY64) > image_size - tls_dir.VirtualAddress) {
        return 0;
    }

    const auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(base + tls_dir.VirtualAddress);
    if (tls->AddressOfCallBacks == 0) {
        return 0;
    }

    // Locate the null-terminated callback pointer table (either inside the mapped image or relocated).
    const uint64_t* cb_table = nullptr;
    uint32_t cb_array_rva = 0;
    size_t max_slots = 256;
    if (MappedVaToRva(tls->AddressOfCallBacks,
                      reinterpret_cast<uintptr_t>(base),
                      nt->OptionalHeader.ImageBase,
                      image_size,
                      cb_array_rva)) {
        cb_table = reinterpret_cast<const uint64_t*>(base + cb_array_rva);
        max_slots = std::min<size_t>(256u, (image_size - cb_array_rva) / sizeof(uint64_t));
    } else {
        cb_table = reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(tls->AddressOfCallBacks));
    }

    uint32_t invoked = 0;
    for (size_t i = 0; i < max_slots; ++i) {
        const uint64_t fn_va = cb_table[i];
        if (fn_va == 0) {
            break;
        }
        PIMAGE_TLS_CALLBACK fn = nullptr;
        uint32_t fn_rva = 0;
        if (MappedVaToRva(fn_va,
                          reinterpret_cast<uintptr_t>(base),
                          nt->OptionalHeader.ImageBase,
                          image_size,
                          fn_rva)) {
            fn = reinterpret_cast<PIMAGE_TLS_CALLBACK>(
                static_cast<uint8_t*>(module_base) + fn_rva);
        } else {
            // Allow direct absolute function pointer (useful for custom/synthetic host-memory harnesses).
            fn = reinterpret_cast<PIMAGE_TLS_CALLBACK>(static_cast<uintptr_t>(fn_va));
        }
        if (fn != nullptr) {
            fn(module_base, static_cast<DWORD>(reason), reserved);
            ++invoked;
        }
    }
    return invoked;
#endif
}

bool RegisterPeExceptionTable(void* module_base, uint32_t pdata_rva, uint32_t pdata_size) {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    if (!module_base || pdata_rva == 0 || pdata_size < sizeof(RUNTIME_FUNCTION)) {
        return false;
    }
    auto* table = reinterpret_cast<PRUNTIME_FUNCTION>(
        static_cast<uint8_t*>(module_base) + pdata_rva);
    const DWORD entry_count = pdata_size / static_cast<DWORD>(sizeof(RUNTIME_FUNCTION));
    if (entry_count == 0) {
        return false;
    }
    return ::RtlAddFunctionTable(table,
                                 entry_count,
                                 reinterpret_cast<DWORD64>(module_base)) != FALSE;
#else
    (void)module_base;
    (void)pdata_rva;
    (void)pdata_size;
    return false;
#endif
}

bool RegisterPeExceptionTableFromImage(void* module_base, uint32_t* out_entry_count) {
    if (out_entry_count) {
        *out_entry_count = 0;
    }
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    const uint8_t* base = nullptr;
    const IMAGE_NT_HEADERS64* nt = nullptr;
    uint32_t image_size = 0;
    if (!ReadMappedPeHeaders64(module_base, base, nt, image_size)) {
        return false;
    }
    const auto& exc_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (exc_dir.VirtualAddress == 0 || exc_dir.Size < sizeof(RUNTIME_FUNCTION) ||
        exc_dir.VirtualAddress >= image_size ||
        exc_dir.Size > image_size - exc_dir.VirtualAddress) {
        return false;
    }
    const uint32_t count = exc_dir.Size / static_cast<uint32_t>(sizeof(RUNTIME_FUNCTION));
    if (!RegisterPeExceptionTable(module_base, exc_dir.VirtualAddress, exc_dir.Size)) {
        return false;
    }
    if (out_entry_count) {
        *out_entry_count = count;
    }
    return true;
#else
    (void)module_base;
    return false;
#endif
}

bool UnregisterPeExceptionTable(void* module_base, uint32_t pdata_rva) {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    if (!module_base || pdata_rva == 0) {
        return false;
    }
    auto* table = reinterpret_cast<PRUNTIME_FUNCTION>(
        static_cast<uint8_t*>(module_base) + pdata_rva);
    return ::RtlDeleteFunctionTable(table) != FALSE;
#else
    (void)module_base;
    (void)pdata_rva;
    return false;
#endif
}

}  // namespace openscarlett
