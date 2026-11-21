#pragma once

#include <cstddef>
#include <cstdint>

namespace openscarlett {

using GpuVirtualAddress = uint64_t;
using GuestHandle = uint64_t;

constexpr uint32_t kPageSize4KB = 4096u;
constexpr uint32_t kPageSize64KB = 65536u;
constexpr uint32_t kPageSize2MB = 2u * 1024u * 1024u;
constexpr uint32_t kPageSize4MB = 4u * 1024u * 1024u;

}  // namespace openscarlett
