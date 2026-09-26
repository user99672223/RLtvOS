// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>

namespace FEXCore::Utils {
// FEX assumes an operating page size of 4096
// To work around build systems that build on a 16k/64k page size, define our page size here
// Don't use the system provided PAGE_SIZE define because of this.
constexpr size_t FEX_PAGE_SIZE = 4096;
constexpr size_t FEX_PAGE_SHIFT = 12;
constexpr size_t FEX_PAGE_MASK = ~(FEX_PAGE_SIZE - 1);

// RLtvOS: granularity of host mprotect/mmap. Guest-facing logic keeps
// FEX_PAGE_SIZE; guard pages and fault pages use the host page size.
#ifdef __APPLE__
constexpr size_t FEX_HOST_PAGE_SIZE = 16384;
#else
constexpr size_t FEX_HOST_PAGE_SIZE = 4096;
#endif
} // namespace FEXCore::Utils
