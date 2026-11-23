#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "openscarlett/launcher/hwtest_runner.h"
#include "openscarlett/loader/pe_inspector.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

#define OS_REQUIRE(cond)                                                                      \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            std::fprintf(stderr, "[FAIL] %s:%d: requirement failed: %s\n", __FILE__,          \
                         __LINE__, #cond);                                                    \
            std::exit(1);                                                                     \
        }                                                                                     \
    } while (0)

uint32_t AlignUp(uint32_t val, uint32_t align) noexcept {
    return (val + align - 1u) & ~(align - 1u);
}

#if defined(_WIN32)

fs::path ResolveRuntimeBinDir() {
    std::error_code ec;
    wchar_t self_buf[MAX_PATH]{};
    const DWORD self_len = ::GetModuleFileNameW(nullptr, self_buf, static_cast<DWORD>(MAX_PATH));
    if (self_len > 0 && self_len < MAX_PATH) {
        const fs::path exe_dir = fs::path(self_buf).parent_path();
        if (fs::exists(exe_dir / L"openscarlett.exe", ec) &&
            fs::exists(exe_dir / L"openscarlett_hook.dll", ec) &&
            fs::exists(exe_dir / L"dxcompiler_xs.dll", ec)) {
            return exe_dir;
        }
    }
    return fs::path(OPENSCARLETT_BINARY_DIR);
}

fs::path ResolveScratchRoot() {
    std::error_code ec;
    wchar_t temp_buf[1024]{};
    const DWORD tlen = ::GetEnvironmentVariableW(L"TEMP", temp_buf, static_cast<DWORD>(1024));
    if (tlen > 0 && tlen < 1024) {
        const fs::path cand = fs::path(temp_buf);
        const std::wstring s = cand.wstring();
        if (!(s.size() >= 2 && (s[0] == L'C' || s[0] == L'c') && s[1] == L':')) {
            return (cand / L"openscarlett_ue5_matrix_bringup").make_preferred();
        }
    }
    return (ResolveRuntimeBinDir() / L".tmp_ue5_matrix_bringup").make_preferred();
}

struct ChildRunResult {
    bool launched = false;
    DWORD exit_code = 0xFFFFFFFFu;
    std::string output;
};

ChildRunResult RunProcessCaptureOutput(const fs::path& exe_path,
                                       const std::wstring& cmd_line,
                                       const fs::path& work_dir,
                                       DWORD timeout_ms = 30000u) {
    ChildRunResult res{};

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE h_read = nullptr;
    HANDLE h_write = nullptr;
    if (!::CreatePipe(&h_read, &h_write, &sa, 0)) {
        return res;
    }
    ::SetHandleInformation(h_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = h_write;
    si.hStdError = h_write;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = cmd_line;
    const BOOL ok = ::CreateProcessW(
        exe_path.c_str(),
        mutable_cmd.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        work_dir.empty() ? nullptr : work_dir.c_str(),
        &si,
        &pi);

    ::CloseHandle(h_write);
    if (!ok) {
        ::CloseHandle(h_read);
        return res;
    }
    res.launched = true;

    char buf[1024]{};
    DWORD nread = 0;
    while (::ReadFile(h_read, buf, sizeof(buf), &nread, nullptr) && nread > 0) {
        res.output.append(buf, nread);
    }
    ::CloseHandle(h_read);

    const DWORD wait_rc = ::WaitForSingleObject(pi.hProcess, timeout_ms);
    if (wait_rc == WAIT_TIMEOUT) {
        ::TerminateProcess(pi.hProcess, 124u);
        ::WaitForSingleObject(pi.hProcess, 2000u);
        res.exit_code = 124u;
    } else {
        DWORD ec = 0;
        if (::GetExitCodeProcess(pi.hProcess, &ec)) {
            res.exit_code = ec;
        }
    }

    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return res;
}

struct DllImportSpec {
    std::string dll_name;
    std::vector<std::string> symbols;
};

// Synthesizes a valid standalone x64 PE32+ executable statically importing symbols from
// all 7 UE5 Matrix Awakens DLLs (dxcompiler_xs, XCRDAPI, xg_xs, d3d12_xs, AcpHal, xmem,
// MFPlat) plus kernel32!ExitProcess.
//
// At runtime, the guest executable:
//   1. Verifies every single IAT slot across all 7 DLLs is non-null.
//   2. Dereferences IID_ID3D12Device data import and checks Data1 == 0x189819F1.
//   3. Calls live APIs across all 7 DLLs:
//        - D3D12XboxSetProcessGpuPriority(1)    -> SUCCEEDED
//        - D3D12XboxSetProcessMemoryPriority(1) -> SUCCEEDED
//        - XMemCheckDefaultHeaps()              -> != 0
//        - ApuCreateHeap(0x100000, 0x100000)    -> SUCCEEDED
//        - MFStartup(0x00020070, 0) + MFShutdown() -> SUCCEEDED
//        - DxcCreateInstance(0, 0, 0)           -> E_POINTER (0x80004003)
//        - DStorageGetFactory(0, 0)             -> E_POINTER (0x80004003)
//        - XGCreateTextureComputer(0, 0)        -> failed HR != E_NOTIMPL
//   4. Calls ExitProcess(0) on complete success, or distinct non-zero failure codes.
bool WriteMatrixAwakensProbeExe(const fs::path& out_path,
                                const std::vector<DllImportSpec>& specs) {
    constexpr uint32_t kFileAlign = 0x200u;
    constexpr uint32_t kSectAlign = 0x1000u;
    constexpr uint32_t kSizeOfHeaders = 0x200u;
    constexpr uint32_t kTextVAddr = 0x1000u;
    constexpr uint32_t kIdataVAddr = 0x2000u;

    // 1. Compute .idata layout dynamically so it scales cleanly to any import count.
    std::vector<uint32_t> iat_offsets(specs.size(), 0u);
    std::vector<uint32_t> int_offsets(specs.size(), 0u);
    std::unordered_map<std::string, uint32_t> sym_iat_rva;
    std::vector<uint32_t> all_xbox_iat_rvas;

    uint32_t cursor = 0u;
    for (size_t i = 0; i < specs.size(); ++i) {
        iat_offsets[i] = cursor;
        for (size_t s = 0; s < specs[i].symbols.size(); ++s) {
            const uint32_t slot_rva = kIdataVAddr + cursor + static_cast<uint32_t>(s * 8u);
            const std::string key = specs[i].dll_name + "!" + specs[i].symbols[s];
            sym_iat_rva[key] = slot_rva;
            if (specs[i].dll_name != "kernel32.dll") {
                all_xbox_iat_rvas.push_back(slot_rva);
            }
        }
        cursor += static_cast<uint32_t>((specs[i].symbols.size() + 1u) * 8u);
    }
    const uint32_t iat_total_size = cursor;

    for (size_t i = 0; i < specs.size(); ++i) {
        int_offsets[i] = cursor;
        cursor += static_cast<uint32_t>((specs[i].symbols.size() + 1u) * 8u);
    }

    const uint32_t desc_offset = AlignUp(cursor, 16u);
    const uint32_t desc_table_size = static_cast<uint32_t>((specs.size() + 1u) * sizeof(IMAGE_IMPORT_DESCRIPTOR));
    const uint32_t str_start = AlignUp(desc_offset + desc_table_size, 16u);

    std::vector<uint8_t> idata(str_start, 0u);
    auto append_cstr = [&](const std::string& s) -> uint32_t {
        if ((idata.size() & 1u) != 0u) idata.push_back(0u);
        const uint32_t rva = kIdataVAddr + static_cast<uint32_t>(idata.size());
        idata.insert(idata.end(), s.begin(), s.end());
        idata.push_back(0u);
        return rva;
    };
    auto append_ibn = [&](const std::string& sym) -> uint32_t {
        if ((idata.size() & 1u) != 0u) idata.push_back(0u);
        const uint32_t rva = kIdataVAddr + static_cast<uint32_t>(idata.size());
        idata.push_back(0u);
        idata.push_back(0u);  // Hint
        idata.insert(idata.end(), sym.begin(), sym.end());
        idata.push_back(0u);
        return rva;
    };

    std::vector<IMAGE_IMPORT_DESCRIPTOR> descs(specs.size() + 1u);
    std::memset(descs.data(), 0, descs.size() * sizeof(IMAGE_IMPORT_DESCRIPTOR));

    for (size_t i = 0; i < specs.size(); ++i) {
        const uint32_t name_rva = append_cstr(specs[i].dll_name);
        std::vector<uint64_t> thunks(specs[i].symbols.size() + 1u, 0ull);
        for (size_t s = 0; s < specs[i].symbols.size(); ++s) {
            thunks[s] = static_cast<uint64_t>(append_ibn(specs[i].symbols[s]));
        }
        std::memcpy(idata.data() + iat_offsets[i], thunks.data(), thunks.size() * sizeof(uint64_t));
        std::memcpy(idata.data() + int_offsets[i], thunks.data(), thunks.size() * sizeof(uint64_t));

        descs[i].OriginalFirstThunk = kIdataVAddr + int_offsets[i];
        descs[i].Name = name_rva;
        descs[i].FirstThunk = kIdataVAddr + iat_offsets[i];
    }
    std::memcpy(idata.data() + desc_offset, descs.data(), descs.size() * sizeof(IMAGE_IMPORT_DESCRIPTOR));

    const uint32_t idata_vsize = static_cast<uint32_t>(idata.size());
    const uint32_t idata_raw_size = AlignUp(idata_vsize, kFileAlign);
    idata.resize(idata_raw_size, 0u);

    // 2. Emit RIP-relative x64 code section.
    std::vector<uint8_t> code;
    auto cur_rva = [&]() -> uint32_t {
        return kTextVAddr + static_cast<uint32_t>(code.size());
    };
    auto emit = [&](std::initializer_list<uint8_t> bs) {
        code.insert(code.end(), bs.begin(), bs.end());
    };
    auto emit_u32 = [&](uint32_t v) {
        const size_t pos = code.size();
        code.resize(pos + 4u);
        std::memcpy(code.data() + pos, &v, 4u);
    };
    auto emit_call_iat = [&](uint32_t iat_rva) {
        emit({0xFF, 0x15});  // call qword ptr [rip + disp32]
        const uint32_t next_rip = cur_rva() + 4u;
        const int32_t disp = static_cast<int32_t>(iat_rva) - static_cast<int32_t>(next_rip);
        emit_u32(static_cast<uint32_t>(disp));
    };
    auto emit_mov_rax_iat = [&](uint32_t iat_rva) {
        emit({0x48, 0x8B, 0x05});  // mov rax, qword ptr [rip + disp32]
        const uint32_t next_rip = cur_rva() + 4u;
        const int32_t disp = static_cast<int32_t>(iat_rva) - static_cast<int32_t>(next_rip);
        emit_u32(static_cast<uint32_t>(disp));
    };

    struct FailJump {
        size_t patch_off = 0u;
        uint32_t exit_code = 1u;
    };
    std::vector<FailJump> fail_jumps;
    auto emit_jcc_fail = [&](uint8_t jcc_op2, uint32_t fail_code) {
        emit({0x0F, jcc_op2});
        fail_jumps.push_back(FailJump{code.size(), fail_code});
        emit_u32(0u);
    };

    const uint32_t iat_exit = sym_iat_rva.at("kernel32.dll!ExitProcess");

    // sub rsp, 0x28 (32-byte shadow space + 16-byte stack alignment)
    emit({0x48, 0x83, 0xEC, 0x28});

    // Check A: Every single IAT slot across all 7 DLLs must be non-null.
    for (size_t i = 0; i < all_xbox_iat_rvas.size(); ++i) {
        emit_mov_rax_iat(all_xbox_iat_rvas[i]);
        emit({0x48, 0x85, 0xC0});  // test rax, rax
        emit_jcc_fail(0x84u, 100u + static_cast<uint32_t>(i));  // je fail
    }

    // Check B: Dereference IID_ID3D12Device data symbol -> Data1 == 0x189819F1.
    emit_mov_rax_iat(sym_iat_rva.at("d3d12_xs.dll!IID_ID3D12Device"));
    emit({0x8B, 0x00});                    // mov eax, dword ptr [rax]
    emit({0x3D, 0xF1, 0x19, 0x98, 0x18});  // cmp eax, 0x189819F1
    emit_jcc_fail(0x85u, 20u);             // jne fail(20)

    // Check C: D3D12XboxSetProcessGpuPriority(1) -> SUCCEEDED (eax >= 0)
    emit({0xB9, 0x01, 0x00, 0x00, 0x00});  // mov ecx, 1
    emit_call_iat(sym_iat_rva.at("d3d12_xs.dll!D3D12XboxSetProcessGpuPriority"));
    emit({0x85, 0xC0});        // test eax, eax
    emit_jcc_fail(0x88u, 21u); // js fail(21)

    // Check D: D3D12XboxSetProcessMemoryPriority(1) -> SUCCEEDED (eax >= 0)
    emit({0xB9, 0x01, 0x00, 0x00, 0x00});  // mov ecx, 1
    emit_call_iat(sym_iat_rva.at("d3d12_xs.dll!D3D12XboxSetProcessMemoryPriority"));
    emit({0x85, 0xC0});        // test eax, eax
    emit_jcc_fail(0x88u, 22u); // js fail(22)

    // Check E: XMemCheckDefaultHeaps() -> != 0
    emit_call_iat(sym_iat_rva.at("xmem.dll!XMemCheckDefaultHeaps"));
    emit({0x85, 0xC0});        // test eax, eax
    emit_jcc_fail(0x84u, 23u); // je fail(23)

    // Check F: ApuCreateHeap(0x100000, 0x100000) -> SUCCEEDED
    emit({0xB9, 0x00, 0x00, 0x10, 0x00});  // mov ecx, 0x100000
    emit({0xBA, 0x00, 0x00, 0x10, 0x00});  // mov edx, 0x100000
    emit_call_iat(sym_iat_rva.at("AcpHal.dll!ApuCreateHeap"));
    emit({0x85, 0xC0});        // test eax, eax
    emit_jcc_fail(0x88u, 24u); // js fail(24)

    // Check G: MFStartup(0x00020070, 0) -> SUCCEEDED, then MFShutdown() -> SUCCEEDED
    emit({0xB9, 0x70, 0x00, 0x02, 0x00});  // mov ecx, 0x00020070 (MF_VERSION)
    emit({0x31, 0xD2});                    // xor edx, edx
    emit_call_iat(sym_iat_rva.at("MFPlat.DLL!MFStartup"));
    emit({0x85, 0xC0});        // test eax, eax
    emit_jcc_fail(0x88u, 25u); // js fail(25)

    emit_call_iat(sym_iat_rva.at("MFPlat.DLL!MFShutdown"));
    emit({0x85, 0xC0});        // test eax, eax
    emit_jcc_fail(0x88u, 26u); // js fail(26)

    // Check H: DxcCreateInstance(nullptr, nullptr, nullptr) -> E_POINTER (0x80004003)
    emit({0x48, 0x31, 0xC9});  // xor rcx, rcx
    emit({0x48, 0x31, 0xD2});  // xor rdx, rdx
    emit({0x4D, 0x31, 0xC0});  // xor r8, r8
    emit_call_iat(sym_iat_rva.at("dxcompiler_xs.dll!DxcCreateInstance"));
    emit({0x3D, 0x03, 0x40, 0x00, 0x80});  // cmp eax, E_POINTER
    emit_jcc_fail(0x85u, 27u);             // jne fail(27)

    // Check I: DStorageGetFactory(nullptr, nullptr) -> E_POINTER (0x80004003) or E_INVALIDARG (0x80070057)
    // Verify FAILED(hr) && hr != E_NOTIMPL.
    emit({0x48, 0x31, 0xC9});  // xor rcx, rcx
    emit({0x48, 0x31, 0xD2});  // xor rdx, rdx
    emit_call_iat(sym_iat_rva.at("XCRDAPI.dll!DStorageGetFactory"));
    emit({0x85, 0xC0});        // test eax, eax
    emit_jcc_fail(0x89u, 28u); // jns fail(28) (must fail with invalid/null args)
    emit({0x3D, 0x01, 0x40, 0x00, 0x80});  // cmp eax, E_NOTIMPL
    emit_jcc_fail(0x84u, 29u);             // je fail(29) (must NOT be E_NOTIMPL)

    // Check J: XGCreateTextureComputer(nullptr, nullptr) -> FAILED(hr) && hr != E_NOTIMPL
    emit({0x48, 0x31, 0xC9});  // xor rcx, rcx
    emit({0x48, 0x31, 0xD2});  // xor rdx, rdx
    emit_call_iat(sym_iat_rva.at("xg_xs.dll!XGCreateTextureComputer"));
    emit({0x85, 0xC0});        // test eax, eax
    emit_jcc_fail(0x89u, 30u); // jns fail(30)
    emit({0x3D, 0x01, 0x40, 0x00, 0x80});  // cmp eax, E_NOTIMPL
    emit_jcc_fail(0x84u, 31u);             // je fail(31)

    // All checks passed -> ExitProcess(0)
    emit({0x31, 0xC9});  // xor ecx, ecx
    emit_call_iat(iat_exit);
    emit({0xCC});

    // Emit failure trampolines
    for (const auto& fj : fail_jumps) {
        const uint32_t label_rva = cur_rva();
        const uint32_t next_rip = kTextVAddr + static_cast<uint32_t>(fj.patch_off) + 4u;
        const int32_t rel = static_cast<int32_t>(label_rva) - static_cast<int32_t>(next_rip);
        std::memcpy(code.data() + fj.patch_off, &rel, 4u);

        emit({0xB9});
        emit_u32(fj.exit_code);
        emit_call_iat(iat_exit);
        emit({0xCC});
    }

    const uint32_t text_vsize = static_cast<uint32_t>(code.size());
    const uint32_t text_raw_size = AlignUp(text_vsize, kFileAlign);
    code.resize(text_raw_size, 0xCCu);
    // Ensure .text stays within 1 page (0x1000) so kIdataVAddr == 0x2000 remains valid.
    OS_REQUIRE(text_vsize < kSectAlign);

    const uint32_t text_raw_ptr = kSizeOfHeaders;
    const uint32_t idata_raw_ptr = text_raw_ptr + text_raw_size;
    const uint32_t size_of_image = kIdataVAddr + AlignUp(idata_vsize, kSectAlign);

    std::vector<uint8_t> pe(kSizeOfHeaders, 0u);
    IMAGE_DOS_HEADER dos{};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = 0x80;
    std::memcpy(pe.data(), &dos, sizeof(dos));

    IMAGE_NT_HEADERS64 nt{};
    nt.Signature = IMAGE_NT_SIGNATURE;
    nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt.FileHeader.NumberOfSections = 2u;
    nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt.FileHeader.Characteristics =
        IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE;

    nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt.OptionalHeader.MajorLinkerVersion = 14u;
    nt.OptionalHeader.SizeOfCode = text_raw_size;
    nt.OptionalHeader.SizeOfInitializedData = idata_raw_size;
    nt.OptionalHeader.AddressOfEntryPoint = kTextVAddr;
    nt.OptionalHeader.BaseOfCode = kTextVAddr;
    nt.OptionalHeader.ImageBase = 0x0000000140000000ull;
    nt.OptionalHeader.SectionAlignment = kSectAlign;
    nt.OptionalHeader.FileAlignment = kFileAlign;
    nt.OptionalHeader.MajorOperatingSystemVersion = 6u;
    nt.OptionalHeader.MajorSubsystemVersion = 6u;
    nt.OptionalHeader.SizeOfImage = size_of_image;
    nt.OptionalHeader.SizeOfHeaders = kSizeOfHeaders;
    nt.OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
    nt.OptionalHeader.DllCharacteristics =
        IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE |
        IMAGE_DLLCHARACTERISTICS_NX_COMPAT |
        IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA;
    nt.OptionalHeader.SizeOfStackReserve = 0x100000ull;
    nt.OptionalHeader.SizeOfStackCommit = 0x1000ull;
    nt.OptionalHeader.SizeOfHeapReserve = 0x100000ull;
    nt.OptionalHeader.SizeOfHeapCommit = 0x1000ull;
    nt.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress =
        kIdataVAddr + desc_offset;
    nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size = desc_table_size;
    nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].VirtualAddress = kIdataVAddr;
    nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].Size = iat_total_size;

    std::memcpy(pe.data() + 0x80, &nt, sizeof(nt));

    IMAGE_SECTION_HEADER secs[2]{};
    std::memcpy(secs[0].Name, ".text", 5);
    secs[0].Misc.VirtualSize = text_vsize;
    secs[0].VirtualAddress = kTextVAddr;
    secs[0].SizeOfRawData = text_raw_size;
    secs[0].PointerToRawData = text_raw_ptr;
    secs[0].Characteristics =
        IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;

    std::memcpy(secs[1].Name, ".idata", 6);
    secs[1].Misc.VirtualSize = idata_vsize;
    secs[1].VirtualAddress = kIdataVAddr;
    secs[1].SizeOfRawData = idata_raw_size;
    secs[1].PointerToRawData = idata_raw_ptr;
    secs[1].Characteristics =
        IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;

    const size_t sec_off = 0x80u + sizeof(IMAGE_NT_HEADERS64);
    std::memcpy(pe.data() + sec_off, secs, sizeof(secs));

    pe.insert(pe.end(), code.begin(), code.end());
    pe.insert(pe.end(), idata.begin(), idata.end());

    std::ofstream ofs(out_path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        return false;
    }
    ofs.write(reinterpret_cast<const char*>(pe.data()), static_cast<std::streamsize>(pe.size()));
    return static_cast<bool>(ofs);
}

#endif  // _WIN32

}  // namespace

int main() {
    std::printf("=== OpenScarlett UE5 Matrix Awakens 7-DLL End-to-End Import Bringup Test ===\n");

#if defined(_WIN32)
    const fs::path bin_dir = ResolveRuntimeBinDir();
    const fs::path openscarlett_exe = bin_dir / L"openscarlett.exe";
    OS_REQUIRE(fs::exists(openscarlett_exe));

    // Confirm all 7 required shim DLLs exist in bin_dir.
    for (const wchar_t* shim_dll : {
             L"dxcompiler_xs.dll",
             L"xcrdapi.dll",
             L"dstorage_xs.dll",
             L"xg_xs.dll",
             L"d3d12_xs.dll",
             L"AcpHal.dll",
             L"xmem.dll",
             L"mfplat_xs.dll",
         }) {
        OS_REQUIRE(fs::exists(bin_dir / shim_dll));
    }

    const fs::path scratch_root = ResolveScratchRoot();
    std::error_code ec;
    fs::remove_all(scratch_root, ec);
    const fs::path guest_dir = scratch_root / L"guest_zero_dlls";
    fs::create_directories(guest_dir, ec);
    OS_REQUIRE(fs::exists(guest_dir));

    const std::vector<DllImportSpec> specs = {
        {"dxcompiler_xs.dll",
         {
             "DxcCreateInstance",
             "DxcCreateInstance2",
         }},
        {"XCRDAPI.dll",
         {
             "DStorageGetFactory",
             "DStorageSetConfiguration",
             "DStorageSetConfiguration1",
         }},
        {"xg_xs.dll",
         {
             "XGCreateTextureComputer",
             "XGCreateTexture1DComputer",
             "XGCreateTexture2DComputer",
             "XGCreateTexture3DComputer",
             "XGComputeOptimalTileMode",
         }},
        {"d3d12_xs.dll",
         {
             "IID_ID3D12Device",
             "IID_ID3D12Device1",
             "IID_ID3D12Device5",
             "IID_ID3D12Device8",
             "IID_ID3D12GraphicsCommandList",
             "IID_ID3D12GraphicsCommandList6",
             "IID_ID3D12CommandQueue",
             "IID_ID3D12Resource",
             "IID_ID3D12Resource2",
             "IID_ID3D12Heap",
             "IID_ID3D12Fence",
             "IID_ID3D12PipelineState",
             "IID_ID3D12RootSignature",
             "IID_ID3D12DescriptorHeap",
             "IID_ID3D12CommandAllocator",
             "IID_ID3D12QueryHeap",
             "IID_ID3D12CommandSignature",
             "IID_ID3D12StateObject",
             "D3D12XboxSetProcessGpuPriority",
             "D3D12XboxSetProcessMemoryPriority",
             "D3D12XboxGetDevice",
             "D3D12XboxSetFrameInterval",
             "D3D12XboxReportError",
         }},
        {"AcpHal.dll",
         {
             "ApuCreateHeap",
             "ApuAlloc",
             "ApuFree",
             "ApuHeapGetState",
             "ApuMapVirtualAddress",
             "ApuMapApuAddress",
             "ApuIsVirtualAddressValid",
         }},
        {"xmem.dll",
         {
             "XMemGetWorkingSetStatistics",
             "XMemGetAuxiliaryTitleMemory",
             "XMemReleaseAuxiliaryTitleMemory",
             "XMemCheckDefaultHeaps",
             "XMemAllocDefault",
             "XMemFreeDefault",
             "XMemSizeDefault",
             "XMemAlloc",
             "XMemFree",
             "XMemSize",
             "XMemSetAllocationHooks",
         }},
        {"MFPlat.DLL",
         {
             "MFStartup",
             "MFShutdown",
             "MFCreateAlignedMemoryBuffer",
             "MFCreateMemoryBuffer",
             "MFCreateSample",
             "MFCreateMediaType",
             "MFCreateAttributes",
             "MFCreateAsyncResult",
             "MFCreateSourceResolver",
             "MFLockWorkQueue",
             "MFUnlockWorkQueue",
             "MFAllocateWorkQueue",
             "MFPutWorkItem",
             "MFCreateWaveFormatExFromMFMediaType",
             "MFInitMediaTypeFromWaveFormatEx",
         }},
        {"kernel32.dll",
         {
             "ExitProcess",
         }},
    };

    const fs::path guest_exe = guest_dir / L"MatrixAwakensImportProbe.exe";
    OS_REQUIRE(WriteMatrixAwakensProbeExe(guest_exe, specs));

    // Verify guest folder has ZERO shim DLLs.
    size_t dll_count_in_guest = 0u;
    for (const auto& entry : fs::directory_iterator(guest_dir)) {
        if (entry.path().extension() == L".dll" || entry.path().extension() == L".DLL") {
            ++dll_count_in_guest;
        }
    }
    OS_REQUIRE(dll_count_in_guest == 0u);

    // 1. Static PE Inspector & Launcher Binary Inspector verification -> 0 missing exports!
    openscarlett::PeInspectionReport pe_rep{};
    const auto pe_st = openscarlett::InspectPeFile(guest_exe.string(), pe_rep);
    OS_REQUIRE(pe_st.ok());
    OS_REQUIRE(pe_rep.is_pe64);
    OS_REQUIRE(pe_rep.imports.size() == specs.size());

    const auto launcher_rep =
        openscarlett::launcher::InspectBinaryForLauncher(guest_exe, bin_dir);
    OS_REQUIRE(launcher_rep.ok);
    OS_REQUIRE(launcher_rep.missing_export_count == 0u);

    for (const auto& mod : launcher_rep.modules) {
        if (!mod.missing_exports.empty()) {
            std::fprintf(stderr, "[FAIL] DLL '%s' (%s) reported %zu missing exports:",
                         mod.dll_name.c_str(), mod.routing_status.c_str(),
                         mod.missing_exports.size());
            for (const auto& sym : mod.missing_exports) {
                std::fprintf(stderr, " %s", sym.c_str());
            }
            std::fprintf(stderr, "\n");
            return 1;
        }
    }
    std::printf("[PASS] Binary Inspector: all 7 UE5 DLLs routed cleanly with 0 missing exports (%u total symbols).\n",
                launcher_rep.total_imports);

    // 2. Live launch via `openscarlett.exe run` (Hardware GPU default)
    {
        const std::wstring cmd =
            L"\"" + openscarlett_exe.wstring() + L"\" run \"" + guest_exe.wstring() + L"\"";
        const ChildRunResult hw = RunProcessCaptureOutput(openscarlett_exe, cmd, guest_dir);
        if (!hw.launched || hw.exit_code != 0u) {
            std::fprintf(stderr, "[FAIL] Hardware run failed (launched=%d exit=%lu):\n%s\n",
                         hw.launched ? 1 : 0, static_cast<unsigned long>(hw.exit_code),
                         hw.output.c_str());
            return 1;
        }
        std::printf("[PASS] Hardware GPU run via openscarlett.exe run: clean boot, all 7 DLLs live, exit 0.\n");
    }

    // 3. Live launch via `openscarlett.exe run` under D3D12 WARP
    {
        ::SetEnvironmentVariableW(L"OPENSCARLETT_PREFER_WARP", L"1");
        const std::wstring cmd =
            L"\"" + openscarlett_exe.wstring() + L"\" run \"" + guest_exe.wstring() + L"\"";
        const ChildRunResult warp = RunProcessCaptureOutput(openscarlett_exe, cmd, guest_dir);
        ::SetEnvironmentVariableW(L"OPENSCARLETT_PREFER_WARP", nullptr);
        if (!warp.launched || warp.exit_code != 0u) {
            std::fprintf(stderr, "[FAIL] WARP run failed (launched=%d exit=%lu):\n%s\n",
                         warp.launched ? 1 : 0, static_cast<unsigned long>(warp.exit_code),
                         warp.output.c_str());
            return 1;
        }
        std::printf("[PASS] D3D12 WARP run via openscarlett.exe run: clean boot, all 7 DLLs live, exit 0.\n");
    }

    fs::remove_all(scratch_root, ec);
    std::printf("[PASS] All UE5 Matrix Awakens 7-DLL end-to-end bringup checks passed!\n");
    return 0;
#else
    std::printf("[SKIP] Windows-only test.\n");
    return 0;
#endif
}
