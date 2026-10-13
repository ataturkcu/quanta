/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_VM_JITPLATFORM_H
#define QUANTA_VM_JITPLATFORM_H

// The only operating-system dependent part of the baseline JIT: where generated
// code lives. Kept in a header of its own, with nothing from the engine in it, so
// that each target's half can be compiled and checked without the rest of the VM.
//
// The calling convention is the other platform dependency, and it is not here: it
// belongs to the code generator (Jit.inc, namespace abi), because the two x86-64
// ABIs differ in which registers carry arguments, how much stack a call must
// reserve and which registers a function must preserve, and all of that is
// decided where the instructions are emitted.

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <unistd.h>
#endif

namespace Quanta::VM::jit::platform {

inline size_t page_size() {
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return static_cast<size_t>(info.dwPageSize);
#else
    return static_cast<size_t>(sysconf(_SC_PAGESIZE));
#endif
}

// Maps `size` bytes (a multiple of page_size()) as writable memory of their own,
// or returns null. Windows commits as it reserves; a mapping is never committed
// without being used, so there is no separate reserve step to manage.
inline void* map_writable(size_t size) {
#if defined(_WIN32)
    return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#endif
}

// Flips a finished mapping to read+execute. Code is written once and is never
// writable while it can run (W^X): a bug that corrupts a heap object cannot be turned
// into new code, and a system that forbids writable+executable pages works unchanged.
inline bool make_executable(void* mem, size_t size) {
#if defined(_WIN32)
    DWORD previous = 0;
    if (!VirtualProtect(mem, size, PAGE_EXECUTE_READ, &previous)) return false;
    // x86 keeps its instruction cache coherent with stores, so this does nothing
    // there; it is the documented step after writing code and costs one call.
    FlushInstructionCache(GetCurrentProcess(), mem, size);
    return true;
#else
    return mprotect(mem, size, PROT_READ | PROT_EXEC) == 0;
#endif
}

inline void unmap(void* mem, size_t size) {
#if defined(_WIN32)
    (void)size;   // MEM_RELEASE frees the whole reservation and requires a size of 0
    VirtualFree(mem, 0, MEM_RELEASE);
#else
    munmap(mem, size);
#endif
}

}  // namespace Quanta::VM::jit::platform

#endif
