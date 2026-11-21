#include "openscarlett/loader/pe_inspector.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <unordered_map>

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

std::string ToLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
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

    uint32_t tls_rva = 0, tls_sz = 0;
    if (read_dir(9, tls_rva, tls_sz) && tls_rva != 0 && tls_sz != 0) {
        out_report.has_tls = true;
    }

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

    // Standard Import Directory (index 1)
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

    // Delay-Load Import Directory (index 13)
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
                if ((attrs & 1u) == 0) continue;  // RVA-based descriptors only (standard PE64)
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

}  // namespace openscarlett
