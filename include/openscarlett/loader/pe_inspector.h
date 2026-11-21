#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "openscarlett/core/status.h"

namespace openscarlett {

enum class DllCategory : uint8_t {
    WindowsNative = 0,
    ConsoleExclusive = 1,
    GdkRuntime = 2,
    Middleware = 3,
    Other = 4,
};

struct PeImportSymbol {
    std::string name;       // Empty when imported by ordinal only
    uint16_t ordinal = 0;
    bool by_ordinal = false;
    bool delay_loaded = false;
};

struct PeImportModule {
    std::string dll_name;
    DllCategory category = DllCategory::Other;
    std::vector<PeImportSymbol> symbols;
};

struct PeInspectionReport {
    std::string file_path;
    uint16_t machine = 0;
    uint16_t subsystem = 0;
    uint64_t image_base = 0;
    uint32_t entry_point_rva = 0;
    uint32_t image_size = 0;
    bool is_pe64 = false;
    bool is_dll = false;
    bool has_relocations = false;
    bool has_tls = false;
    uint32_t section_count = 0;
    uint32_t total_imports = 0;
    uint32_t console_imports = 0;
    uint32_t gdk_imports = 0;
    uint32_t delay_load_imports = 0;
    std::vector<PeImportModule> imports;
};

const char* DllCategoryName(DllCategory category) noexcept;
DllCategory ClassifyImportDll(const std::string& dll_name);

Status InspectPeFile(const std::string& file_path, PeInspectionReport& out_report);
Status InspectPeBytes(const std::vector<uint8_t>& bytes,
                      const std::string& label,
                      PeInspectionReport& out_report);

}  // namespace openscarlett
