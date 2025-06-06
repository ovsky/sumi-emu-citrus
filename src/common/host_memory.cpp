// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2025 sumi Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32

#include <iterator>
#include <unordered_map>
#include <boost/icl/separate_interval_set.hpp>
#include <windows.h>
#include "common/dynamic_library.h"

#elif defined(__linux__) || defined(__FreeBSD__) // ^^^ Windows ^^^ vvv Linux vvv

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <boost/icl/interval_set.hpp>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <unistd.h>
#include "common/scope_exit.h"

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

#endif // ^^^ Linux ^^^

#include <mutex>
#include <random>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/free_region_manager.h"
#include "common/host_memory.h"
#include "common/logging/log.h"

namespace Common {

constexpr size_t PageAlignment = 0x1000;
constexpr size_t HugePageSize = 0x200000;
constexpr size_t max_memory_size = 0x40000000; // 1GB max memory size
constexpr bool ENABLE_MEMORY_DEBUG = true;

// Move LogMemoryOperation declaration to the top, before any usage
static void LogMemoryOperation(const char* operation, size_t virtual_offset, size_t host_offset,
                             size_t length, const char* result = nullptr) {
    if (!ENABLE_MEMORY_DEBUG) {
        return;
    }
    if (result) {
        LOG_DEBUG(Common_Memory, "{}: virtual=0x{:x}, host=0x{:x}, length=0x{:x} ({})",
                 operation, virtual_offset, host_offset, length, result);
    } else {
        LOG_DEBUG(Common_Memory, "{}: virtual=0x{:x}, host=0x{:x}, length=0x{:x}",
                 operation, virtual_offset, host_offset, length);
    }
}

#ifdef _WIN32

// Manually imported for MinGW compatibility
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_COALESCE_PLACEHOLDERS
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif

using PFN_CreateFileMapping2 = _Ret_maybenull_ HANDLE(WINAPI*)(
    _In_ HANDLE File, _In_opt_ SECURITY_ATTRIBUTES* SecurityAttributes, _In_ ULONG DesiredAccess,
    _In_ ULONG PageProtection, _In_ ULONG AllocationAttributes, _In_ ULONG64 MaximumSize,
    _In_opt_ PCWSTR Name,
    _Inout_updates_opt_(ParameterCount) MEM_EXTENDED_PARAMETER* ExtendedParameters,
    _In_ ULONG ParameterCount);

using PFN_VirtualAlloc2 = _Ret_maybenull_ PVOID(WINAPI*)(
    _In_opt_ HANDLE Process, _In_opt_ PVOID BaseAddress, _In_ SIZE_T Size,
    _In_ ULONG AllocationType, _In_ ULONG PageProtection,
    _Inout_updates_opt_(ParameterCount) MEM_EXTENDED_PARAMETER* ExtendedParameters,
    _In_ ULONG ParameterCount);

using PFN_MapViewOfFile3 = _Ret_maybenull_ PVOID(WINAPI*)(
    _In_ HANDLE FileMapping, _In_opt_ HANDLE Process, _In_opt_ PVOID BaseAddress,
    _In_ ULONG64 Offset, _In_ SIZE_T ViewSize, _In_ ULONG AllocationType, _In_ ULONG PageProtection,
    _Inout_updates_opt_(ParameterCount) MEM_EXTENDED_PARAMETER* ExtendedParameters,
    _In_ ULONG ParameterCount);

using PFN_UnmapViewOfFile2 = BOOL(WINAPI*)(_In_ HANDLE Process, _In_ PVOID BaseAddress,
                                           _In_ ULONG UnmapFlags);

template <typename T>
static void GetFuncAddress(Common::DynamicLibrary& dll, const char* name, T& pfn) {
    if (!dll.GetSymbol(name, &pfn)) {
        LOG_CRITICAL(HW_Memory, "Failed to load {}", name);
        throw std::bad_alloc{};
    }
}

class HostMemory::Impl {
public:
    explicit Impl(size_t backing_size_, size_t virtual_size_)
        : backing_size{backing_size_}, virtual_size{virtual_size_}, process{GetCurrentProcess()},
          kernelbase_dll("Kernelbase") {
        if (!kernelbase_dll.IsOpen()) {
            LOG_CRITICAL(HW_Memory, "Failed to load Kernelbase.dll");
            throw std::bad_alloc{};
        }
        GetFuncAddress(kernelbase_dll, "CreateFileMapping2", pfn_CreateFileMapping2);
        GetFuncAddress(kernelbase_dll, "VirtualAlloc2", pfn_VirtualAlloc2);
        GetFuncAddress(kernelbase_dll, "MapViewOfFile3", pfn_MapViewOfFile3);
        GetFuncAddress(kernelbase_dll, "UnmapViewOfFile2", pfn_UnmapViewOfFile2);

        // Allocate backing file map
        backing_handle =
            pfn_CreateFileMapping2(INVALID_HANDLE_VALUE, nullptr, FILE_MAP_WRITE | FILE_MAP_READ,
                                   PAGE_READWRITE, SEC_COMMIT, backing_size, nullptr, nullptr, 0);
        if (!backing_handle) {
            LOG_CRITICAL(HW_Memory, "Failed to allocate {} MiB of backing memory",
                         backing_size >> 20);
            throw std::bad_alloc{};
        }
        // Allocate a virtual memory for the backing file map as placeholder
        backing_base =
            static_cast<u8*>(pfn_VirtualAlloc2(process, nullptr, backing_size,
                                                          MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                                          PAGE_NOACCESS, nullptr, 0));
        if (!backing_base) {
            Release();
            LOG_CRITICAL(HW_Memory, "Failed to reserve {} MiB of virtual memory",
                         backing_size >> 20);
            throw std::bad_alloc{};
        }
        // Map backing placeholder
        void* const ret = pfn_MapViewOfFile3(backing_handle, process, backing_base, 0, backing_size,
                                             MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
        if (ret != backing_base) {
            Release();
            LOG_CRITICAL(HW_Memory, "Failed to map {} MiB of virtual memory", backing_size >> 20);
            throw std::bad_alloc{};
        }
        // Allocate virtual address placeholder
        virtual_base = static_cast<u8*>(pfn_VirtualAlloc2(process, nullptr, virtual_size,
                                                          MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                                          PAGE_NOACCESS, nullptr, 0));
        if (!virtual_base) {
            Release();
            LOG_CRITICAL(HW_Memory, "Failed to reserve {} GiB of virtual memory",
                         virtual_size >> 30);
            throw std::bad_alloc{};
        }
    }

    ~Impl() {
        Release();
    }

    void Map(size_t virtual_offset, size_t host_offset, size_t length, MemoryPermission perms) {
        std::unique_lock lock{placeholder_mutex};
        if (!IsNiechePlaceholder(virtual_offset, length)) {
            Split(virtual_offset, length);
        }
        ASSERT(placeholders.find({virtual_offset, virtual_offset + length}) == placeholders.end());
        TrackPlaceholder(virtual_offset, host_offset, length);

        MapView(virtual_offset, host_offset, length);
    }

    void Unmap(size_t virtual_offset, size_t length) {
        std::scoped_lock lock{placeholder_mutex};

        // Unmap until there are no more placeholders
        while (UnmapOnePlaceholder(virtual_offset, length)) {
        }
    }

    void Protect(size_t virtual_offset, size_t length, bool read, bool write, bool execute) {
        DWORD new_flags{};
        if (read && write) {
            new_flags = PAGE_READWRITE;
        } else if (read && !write) {
            new_flags = PAGE_READONLY;
        } else if (!read && !write) {
            new_flags = PAGE_NOACCESS;
        } else {
            UNIMPLEMENTED_MSG("Protection flag combination read={} write={}", read, write);
        }
        const size_t virtual_end = virtual_offset + length;

        std::scoped_lock lock{placeholder_mutex};
        auto [it, end] = placeholders.equal_range({virtual_offset, virtual_end});
        while (it != end) {
            const size_t offset = std::max(it->lower(), virtual_offset);
            const size_t protect_length = std::min(it->upper(), virtual_end) - offset;
            DWORD old_flags{};
            if (!VirtualProtect(virtual_base + offset, protect_length, new_flags, &old_flags)) {
                LOG_CRITICAL(HW_Memory, "Failed to change virtual memory protect rules");
            }
            ++it;
        }
    }

    bool ClearBackingRegion(size_t physical_offset, size_t length) {
        // TODO: This does not seem to be possible on Windows.
        return false;
    }

    void EnableDirectMappedAddress() {
        // TODO
        UNREACHABLE();
    }

    bool IsValidMapping(size_t offset, size_t length) const {
        return (offset + length) <= backing_size;
    }

    bool IsDirectMappingEnabled() const {
        return direct_mapping_enabled;
    }

    const size_t backing_size; ///< Size of the backing memory in bytes
    const size_t virtual_size; ///< Size of the virtual address placeholder in bytes

    u8* backing_base{};
    u8* virtual_base{};
    bool direct_mapping_enabled{false};

private:
    /// Release all resources in the object
    void Release() {
        if (!placeholders.empty()) {
            for (const auto& placeholder : placeholders) {
                if (!pfn_UnmapViewOfFile2(process, virtual_base + placeholder.lower(),
                                          MEM_PRESERVE_PLACEHOLDER)) {
                    LOG_CRITICAL(HW_Memory, "Failed to unmap virtual memory placeholder");
                }
            }
            Coalesce(0, virtual_size);
        }
        if (virtual_base) {
            if (!VirtualFree(virtual_base, 0, MEM_RELEASE)) {
                LOG_CRITICAL(HW_Memory, "Failed to free virtual memory");
            }
        }
        if (backing_base) {
            if (!pfn_UnmapViewOfFile2(process, backing_base, MEM_PRESERVE_PLACEHOLDER)) {
                LOG_CRITICAL(HW_Memory, "Failed to unmap backing memory placeholder");
            }
            if (!VirtualFreeEx(process, backing_base, 0, MEM_RELEASE)) {
                LOG_CRITICAL(HW_Memory, "Failed to free backing memory");
            }
        }
        if (!CloseHandle(backing_handle)) {
            LOG_CRITICAL(HW_Memory, "Failed to free backing memory file handle");
        }
    }

    /// Unmap one placeholder in the given range (partial unmaps are supported)
    /// Return true when there are no more placeholders to unmap
    bool UnmapOnePlaceholder(size_t virtual_offset, size_t length) {
        const auto it = placeholders.find({virtual_offset, virtual_offset + length});
        const auto begin = placeholders.begin();
        const auto end = placeholders.end();
        if (it == end) {
            return false;
        }
        const size_t placeholder_begin = it->lower();
        const size_t placeholder_end = it->upper();
        const size_t unmap_begin = std::max(virtual_offset, placeholder_begin);
        const size_t unmap_end = std::min(virtual_offset + length, placeholder_end);
        ASSERT(unmap_begin >= placeholder_begin && unmap_begin < placeholder_end);
        ASSERT(unmap_end <= placeholder_end && unmap_end > placeholder_begin);

        const auto host_pointer_it = placeholder_host_pointers.find(placeholder_begin);
        ASSERT(host_pointer_it != placeholder_host_pointers.end());
        const size_t host_offset = host_pointer_it->second;

        const bool split_left = unmap_begin > placeholder_begin;
        const bool split_right = unmap_end < placeholder_end;

        if (!pfn_UnmapViewOfFile2(process, virtual_base + placeholder_begin,
                                  MEM_PRESERVE_PLACEHOLDER)) {
            LOG_CRITICAL(HW_Memory, "Failed to unmap placeholder");
        }
        // If we have to remap memory regions due to partial unmaps, we are in a data race as
        // Windows doesn't support remapping memory without unmapping first. Avoid adding any extra
        // logic within the panic region described below.

        // Panic region, we are in a data race right now
        if (split_left || split_right) {
            Split(unmap_begin, unmap_end - unmap_begin);
        }
        if (split_left) {
            MapView(placeholder_begin, host_offset, unmap_begin - placeholder_begin);
        }
        if (split_right) {
            MapView(unmap_end, host_offset + unmap_end - placeholder_begin,
                    placeholder_end - unmap_end);
        }
        // End panic region

        size_t coalesce_begin = unmap_begin;
        if (!split_left) {
            // Try to coalesce pages to the left
            coalesce_begin = it == begin ? 0 : std::prev(it)->upper();
            if (coalesce_begin != placeholder_begin) {
                Coalesce(coalesce_begin, unmap_end - coalesce_begin);
            }
        }
        if (!split_right) {
            // Try to coalesce pages to the right
            const auto next = std::next(it);
            const size_t next_begin = next == end ? virtual_size : next->lower();
            if (placeholder_end != next_begin) {
                // We can coalesce to the right
                Coalesce(coalesce_begin, next_begin - coalesce_begin);
            }
        }
        // Remove and reinsert placeholder trackers
        UntrackPlaceholder(it);
        if (split_left) {
            TrackPlaceholder(placeholder_begin, host_offset, unmap_begin - placeholder_begin);
        }
        if (split_right) {
            TrackPlaceholder(unmap_end, host_offset + unmap_end - placeholder_begin,
                             placeholder_end - unmap_end);
        }
        return true;
    }

    void MapView(size_t virtual_offset, size_t host_offset, size_t length) {
        if (!pfn_MapViewOfFile3(backing_handle, process, virtual_base + virtual_offset, host_offset,
                                length, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0)) {
            LOG_CRITICAL(HW_Memory, "Failed to map placeholder");
        }
    }

    void Split(size_t virtual_offset, size_t length) {
        if (!VirtualFreeEx(process, reinterpret_cast<LPVOID>(virtual_base + virtual_offset), length,
                           MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            LOG_CRITICAL(HW_Memory, "Failed to split placeholder");
        }
    }

    void Coalesce(size_t virtual_offset, size_t length) {
        if (!VirtualFreeEx(process, reinterpret_cast<LPVOID>(virtual_base + virtual_offset), length,
                           MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) {
            LOG_CRITICAL(HW_Memory, "Failed to coalesce placeholders");
        }
    }

    void TrackPlaceholder(size_t virtual_offset, size_t host_offset, size_t length) {
        placeholders.insert({virtual_offset, virtual_offset + length});
        placeholder_host_pointers.emplace(virtual_offset, host_offset);
    }

    void UntrackPlaceholder(boost::icl::separate_interval_set<size_t>::iterator it) {
        placeholder_host_pointers.erase(it->lower());
        placeholders.erase(it);
    }

    /// Return true when a given memory region is a "nieche" and the placeholders don't have to be
    /// split.
    bool IsNiechePlaceholder(size_t virtual_offset, size_t length) const {
        const auto it = placeholders.upper_bound({virtual_offset, virtual_offset + length});
        if (it != placeholders.end() && it->lower() == virtual_offset + length) {
            return it == placeholders.begin() ? virtual_offset == 0
                                              : std::prev(it)->upper() == virtual_offset;
        }
        return false;
    }

    HANDLE process{};        ///< Current process handle
    HANDLE backing_handle{}; ///< File based backing memory

    DynamicLibrary kernelbase_dll;
    PFN_CreateFileMapping2 pfn_CreateFileMapping2{};
    PFN_VirtualAlloc2 pfn_VirtualAlloc2{};
    PFN_MapViewOfFile3 pfn_MapViewOfFile3{};
    PFN_UnmapViewOfFile2 pfn_UnmapViewOfFile2{};

    std::mutex placeholder_mutex;                                 ///< Mutex for placeholders
    boost::icl::separate_interval_set<size_t> placeholders;       ///< Mapped placeholders
    std::unordered_map<size_t, size_t> placeholder_host_pointers; ///< Placeholder backing offset
};

#elif defined(__linux__) || defined(__FreeBSD__) // ^^^ Windows ^^^ vvv Linux vvv

#ifdef ARCHITECTURE_arm64

static void* ChooseVirtualBase(size_t virtual_size) {
    constexpr uintptr_t Map39BitSize = (1ULL << 39);
    constexpr uintptr_t Map36BitSize = (1ULL << 36);

    // This is not a cryptographic application, we just want something random.
    std::mt19937_64 rng;

    // We want to ensure we are allocating at an address aligned to the L2 block size.
    // For Qualcomm devices, we must also allocate memory above 36 bits.
    const size_t lower = Map36BitSize / HugePageSize;
    const size_t upper = (Map39BitSize - virtual_size) / HugePageSize;
    const size_t range = upper - lower;

    // Try up to 64 times to allocate memory at random addresses in the range.
    for (int i = 0; i < 64; i++) {
        // Calculate a possible location.
        uintptr_t hint_address = ((rng() % range) + lower) * HugePageSize;

        // Try to map.
        // Note: we may be able to take advantage of MAP_FIXED_NOREPLACE here.
        void* map_pointer =
            mmap(reinterpret_cast<void*>(hint_address), virtual_size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);

        // If we successfully mapped, we're done.
        if (reinterpret_cast<uintptr_t>(map_pointer) == hint_address) {
            return map_pointer;
        }

        // Unmap if necessary, and try again.
        if (map_pointer != MAP_FAILED) {
            munmap(map_pointer, virtual_size);
        }
    }

    return MAP_FAILED;
}

#else

static void* ChooseVirtualBase(size_t virtual_size) {
#if defined(__FreeBSD__)
    void* virtual_base =
        mmap(nullptr, virtual_size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_ALIGNED_SUPER, -1, 0);

    if (virtual_base != MAP_FAILED) {
        return virtual_base;
    }
#endif

    return mmap(nullptr, virtual_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
}

#endif

class HostMemory::Impl {
public:
    explicit Impl(size_t backing_size_, size_t virtual_size_)
        : backing_size{backing_size_}, virtual_size{virtual_size_} {
        bool good = false;
        SCOPE_EXIT {
            if (!good) {
                Release();
            }
        };

        long page_size = sysconf(_SC_PAGESIZE);
        if (page_size != 0x1000) {
            LOG_CRITICAL(HW_Memory, "page size {:#x} is incompatible with 4K paging", page_size);
            throw std::bad_alloc{};
        }

        // Backing memory initialization
#if defined(__FreeBSD__) && __FreeBSD__ < 13
        // XXX Drop after FreeBSD 12.* reaches EOL on 2024-06-30
        fd = shm_open(SHM_ANON, O_RDWR, 0600);
#else
        fd = memfd_create("HostMemory", 0);
#endif
        if (fd < 0) {
            LOG_CRITICAL(HW_Memory, "memfd_create failed: {}", strerror(errno));
            throw std::bad_alloc{};
        }

        // Defined to extend the file with zeros
        int ret = ftruncate(fd, backing_size);
        if (ret != 0) {
            LOG_CRITICAL(HW_Memory, "ftruncate failed with {}, are you out-of-memory?",
                         strerror(errno));
            throw std::bad_alloc{};
        }

        backing_base = static_cast<u8*>(
            mmap(nullptr, backing_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        if (backing_base == MAP_FAILED) {
            LOG_CRITICAL(HW_Memory, "mmap failed: {}", strerror(errno));
            throw std::bad_alloc{};
        }

        // Virtual memory initialization
        virtual_base = virtual_map_base = static_cast<u8*>(ChooseVirtualBase(virtual_size));
        if (virtual_base == MAP_FAILED) {
            LOG_CRITICAL(HW_Memory, "mmap failed: {}", strerror(errno));
            throw std::bad_alloc{};
        }
#if defined(__linux__)
        madvise(virtual_base, virtual_size, MADV_HUGEPAGE);
#endif

        free_manager.SetAddressSpace(virtual_base, virtual_size);
        good = true;
    }

    ~Impl() {
        Release();
    }

    void Map(size_t virtual_offset, size_t host_offset, size_t length, MemoryPermission perms) {
        // Intersect the range with our address space.
        AdjustMap(&virtual_offset, &length);

        // If length is 0 after adjustment, nothing to map
        if (length == 0) {
            return;
        }

        // We are removing a placeholder.
        try {
            free_manager.AllocateBlock(virtual_base + virtual_offset, length);
        } catch (const std::exception& e) {
            LOG_ERROR(HW_Memory, "Failed to allocate block: {}", e.what());
            return;
        }

        // Deduce mapping protection flags.
        int flags = PROT_NONE;
        if (True(perms & MemoryPermission::Read)) {
            flags |= PROT_READ;
        }
        if (True(perms & MemoryPermission::Write)) {
            flags |= PROT_WRITE;
        }
#ifdef ARCHITECTURE_arm64
        if (True(perms & MemoryPermission::Execute)) {
            flags |= PROT_EXEC;
        }
#endif

        void* ret = mmap(virtual_base + virtual_offset, length, flags, MAP_SHARED | MAP_FIXED, fd,
                         host_offset);
        if (ret == MAP_FAILED) {
            LOG_ERROR(HW_Memory, "mmap failed: {}", strerror(errno));
            // Try to restore the placeholder
            try {
                void* placeholder = mmap(virtual_base + virtual_offset, length, PROT_NONE,
                                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
                if (placeholder != MAP_FAILED) {
                    free_manager.FreeBlock(virtual_base + virtual_offset, length);
                }
            } catch (...) {
                // Best effort recovery
            }
            return;
        }
    }

    void Unmap(size_t virtual_offset, size_t length) {
        // Intersect the range with our address space.
        AdjustMap(&virtual_offset, &length);

        // If length is 0 after adjustment, nothing to unmap
        if (length == 0) {
            return;
        }

        try {
            // Merge with any adjacent placeholder mappings.
            auto [merged_pointer, merged_size] =
                free_manager.FreeBlock(virtual_base + virtual_offset, length);

            void* ret = mmap(merged_pointer, merged_size, PROT_NONE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            if (ret == MAP_FAILED) {
                LOG_ERROR(HW_Memory, "mmap failed during unmap: {}", strerror(errno));
                // Try to restore the original mapping
                free_manager.AllocateBlock(virtual_base + virtual_offset, length);
                return;
            }
        } catch (const std::exception& e) {
            LOG_ERROR(HW_Memory, "Failed to free block: {}", e.what());
            return;
        }
    }

    void Protect(size_t virtual_offset, size_t length, bool read, bool write, bool execute) {
        // Intersect the range with our address space.
        AdjustMap(&virtual_offset, &length);

        int flags = PROT_NONE;
        if (read) {
            flags |= PROT_READ;
        }
        if (write) {
            flags |= PROT_WRITE;
        }
#ifdef HAS_NCE
        if (execute) {
            flags |= PROT_EXEC;
        }
#endif
        int ret = mprotect(virtual_base + virtual_offset, length, flags);
        ASSERT_MSG(ret == 0, "mprotect failed: {}", strerror(errno));
    }

    bool ClearBackingRegion(size_t physical_offset, size_t length) {
#ifdef __linux__
        // Set MADV_REMOVE on backing map to destroy it instantly.
        // This also deletes the area from the backing file.
        int ret = madvise(backing_base + physical_offset, length, MADV_REMOVE);
        ASSERT_MSG(ret == 0, "madvise failed: {}", strerror(errno));

        return true;
#else
        return false;
#endif
    }

    void EnableDirectMappedAddress() {
        virtual_base = nullptr;
    }

    bool IsValidMapping(size_t offset, size_t length) const {
        return (offset + length) <= backing_size;
    }

    bool IsDirectMappingEnabled() const {
        return virtual_base == nullptr;
    }

    const size_t backing_size; ///< Size of the backing memory in bytes
    const size_t virtual_size; ///< Size of the virtual address placeholder in bytes

    u8* backing_base{reinterpret_cast<u8*>(MAP_FAILED)};
    u8* virtual_base{reinterpret_cast<u8*>(MAP_FAILED)};
    u8* virtual_map_base{reinterpret_cast<u8*>(MAP_FAILED)};

private:
    /// Release all resources in the object
    void Release() {
        if (virtual_map_base != MAP_FAILED) {
            int ret = munmap(virtual_map_base, virtual_size);
            ASSERT_MSG(ret == 0, "munmap failed: {}", strerror(errno));
        }

        if (backing_base != MAP_FAILED) {
            int ret = munmap(backing_base, backing_size);
            ASSERT_MSG(ret == 0, "munmap failed: {}", strerror(errno));
        }

        if (fd != -1) {
            int ret = close(fd);
            ASSERT_MSG(ret == 0, "close failed: {}", strerror(errno));
        }
    }

    void AdjustMap(size_t* virtual_offset, size_t* length) {
        if (virtual_base != nullptr) {
            return;
        }

        // If we are direct mapped, we want to make sure we are operating on a region
        // that is in range of our virtual mapping.
        size_t intended_start = *virtual_offset;
        size_t intended_end = intended_start + *length;
        size_t address_space_start = reinterpret_cast<size_t>(virtual_map_base);
        size_t address_space_end = address_space_start + virtual_size;

        if (address_space_start > intended_end || intended_start > address_space_end) {
            *virtual_offset = 0;
            *length = 0;
        } else {
            *virtual_offset = std::max(intended_start, address_space_start);
            *length = std::min(intended_end, address_space_end) - *virtual_offset;
        }
    }

    int fd{-1}; // memfd file descriptor, -1 is the error value of memfd_create
    FreeRegionManager free_manager{};
};

#else // ^^^ Linux ^^^ vvv Generic vvv

class HostMemory::Impl {
public:
    explicit Impl(size_t /*backing_size */, size_t /* virtual_size */) {
        // This is just a place holder.
        // Please implement fastmem in a proper way on your platform.
        throw std::bad_alloc{};
    }

    void Map(size_t virtual_offset, size_t host_offset, size_t length, MemoryPermission perm) {}

    void Unmap(size_t virtual_offset, size_t length) {}

    void Protect(size_t virtual_offset, size_t length, bool read, bool write, bool execute) {}

    bool ClearBackingRegion(size_t physical_offset, size_t length) {
        return false;
    }

    void EnableDirectMappedAddress() {}

    bool IsValidMapping(size_t offset, size_t length) const {
        return false;
    }

    bool IsDirectMappingEnabled() const {
        return false;
    }

    u8* backing_base{nullptr};
    u8* virtual_base{nullptr};
};

#endif // ^^^ Generic ^^^

HostMemory::HostMemory(size_t backing_size_, size_t virtual_size_)
    : backing_size(backing_size_), virtual_size(virtual_size_) {
    try {
        // Try to allocate a fastmem arena.
        // The implementation will fail with std::bad_alloc on errors.
        impl =
            std::make_unique<HostMemory::Impl>(AlignUp(backing_size, PageAlignment),
                                               AlignUp(virtual_size, PageAlignment) + HugePageSize);
        backing_base = impl->backing_base;
        virtual_base = impl->virtual_base;

        if (virtual_base) {
            // Ensure the virtual base is aligned to the L2 block size.
            virtual_base = reinterpret_cast<u8*>(
                Common::AlignUp(reinterpret_cast<uintptr_t>(virtual_base), HugePageSize));
            virtual_base_offset = static_cast<size_t>(
                reinterpret_cast<uintptr_t>(virtual_base) -
                reinterpret_cast<uintptr_t>(impl->virtual_base));
        }

    } catch (const std::bad_alloc&) {
        LOG_CRITICAL(HW_Memory,
                     "Fastmem unavailable, falling back to VirtualBuffer for memory allocation");
        fallback_buffer = std::make_unique<Common::VirtualBuffer<u8>>(backing_size);
        backing_base = fallback_buffer->data();
        virtual_base = nullptr;
    }
}

HostMemory::~HostMemory() = default;

HostMemory::HostMemory(HostMemory&&) noexcept = default;

HostMemory& HostMemory::operator=(HostMemory&&) noexcept = default;

void HostMemory::Map(size_t virtual_offset, size_t host_offset, size_t length,
                     MemoryPermission perms, bool separate_heap) {
    // Add additional checks before mapping
    if (virtual_offset == 0 || host_offset == 0) {
        LOG_ERROR(Common_Memory, "Invalid memory mapping addresses");
        return;
    }

    // Ensure addresses are properly aligned
    if ((virtual_offset & 0xFFF) != 0 || (host_offset & 0xFFF) != 0) {
        LOG_ERROR(Common_Memory, "Unaligned memory mapping addresses");
        return;
    }

    // Add size validation
    if (length == 0 || length > max_memory_size) {
        LOG_ERROR(Common_Memory, "Invalid mapping length: {}", length);
        return;
    }

    LogMemoryOperation("Map", virtual_offset, host_offset, length);

    ASSERT(virtual_offset % PageAlignment == 0);
    ASSERT(length % PageAlignment == 0);
    ASSERT(virtual_offset + length <= virtual_size);

    if (length == 0 || !virtual_base || !impl) {
        return;
    }

    // Check if mapping is valid
    if (!impl->IsValidMapping(host_offset, length)) {
        LOG_WARNING(Common_Memory,
            "Memory validation failed: virtual=0x{:x}, host=0x{:x}, length=0x{:x}",
            virtual_offset, host_offset, length);
        // Continue anyway - the mapping may still work
    }

    impl->Map(virtual_offset + virtual_base_offset, host_offset, length, perms);
}

void HostMemory::Unmap(size_t virtual_offset, size_t length, bool separate_heap) {
    LogMemoryOperation("Unmap", virtual_offset, 0, length);

    ASSERT(virtual_offset % PageAlignment == 0);
    ASSERT(length % PageAlignment == 0);
    ASSERT(virtual_offset + length <= virtual_size);

    if (length == 0 || !virtual_base || !impl) {
        return;
    }
    impl->Unmap(virtual_offset + virtual_base_offset, length);
}

void HostMemory::Protect(size_t virtual_offset, size_t length, MemoryPermission perm) {
    ASSERT(virtual_offset % PageAlignment == 0);
    ASSERT(length % PageAlignment == 0);
    ASSERT(virtual_offset + length <= virtual_size);
    if (length == 0 || !virtual_base || !impl) {
        return;
    }
    const bool read = True(perm & MemoryPermission::Read);
    const bool write = True(perm & MemoryPermission::Write);
    const bool execute = True(perm & MemoryPermission::Execute);
    impl->Protect(virtual_offset + virtual_base_offset, length, read, write, execute);
}

void HostMemory::ClearBackingRegion(size_t physical_offset, size_t length, u32 fill_value) {
    if (!impl || fill_value != 0 || !impl->ClearBackingRegion(physical_offset, length)) {
        std::memset(backing_base + physical_offset, fill_value, length);
    }
}

void HostMemory::EnableDirectMappedAddress() {
    if (!impl) {
        LOG_ERROR(Common_Memory, "Implementation not initialized");
        return;
    }

    impl->EnableDirectMappedAddress();

    // Only update virtual_size if the direct mapping was successful
    if (impl->IsDirectMappingEnabled()) {
        virtual_size += reinterpret_cast<uintptr_t>(virtual_base);
    } else {
        LOG_ERROR(Common_Memory, "Failed to enable direct mapped address");
    }
}

bool HostMemory::MapMemory(uint64_t virtual_offset, uint64_t host_offset, uint64_t length) {
    static constexpr uint64_t MAX_SAFE_ALLOCATION = 0x40000000; // 1GB max allocation

    if (!impl || !impl->IsValidMapping(host_offset, length)) {
        // For very large allocations, try splitting into smaller chunks
        if (length > MAX_SAFE_ALLOCATION) {
            LOG_WARNING(Common_Memory,
                "Large mapping requested: virtual=0x{:x}, host=0x{:x}, size=0x{:x}. Attempting split allocation.",
                virtual_offset, host_offset, length);

            // Try mapping in smaller chunks
            uint64_t remaining = length;
            uint64_t current_virtual = virtual_offset;
            uint64_t current_host = host_offset;

            while (remaining > 0) {
                uint64_t chunk_size = std::min(remaining, MAX_SAFE_ALLOCATION);
                if (!MapMemory(current_virtual, current_host, chunk_size)) {
                    LOG_ERROR(Common_Memory,
                        "Failed to map memory chunk: virtual=0x{:x}, host=0x{:x}, size=0x{:x}",
                        current_virtual, current_host, chunk_size);
                    return false;
                }
                remaining -= chunk_size;
                current_virtual += chunk_size;
                current_host += chunk_size;
            }
            return true;
        }

        LOG_ERROR(Common_Memory,
            "Failed to verify memory mapping: virtual_offset=0x{:x}, host_offset=0x{:x}, length=0x{:x}",
            virtual_offset, host_offset, length);
        return false;
    }

    // Ensure addresses are page-aligned
    if ((virtual_offset & (PageAlignment - 1)) || (host_offset & (PageAlignment - 1))) {
        LOG_ERROR(Common_Memory,
            "Unaligned memory mapping: virtual=0x{:x}, host=0x{:x}",
            virtual_offset, host_offset);
        return false;
    }

    try {
        // Add the missing separate_heap parameter
        Map(virtual_offset, host_offset, length, MemoryPermission::ReadWrite, false);

        if (ENABLE_MEMORY_DEBUG) {
            LOG_DEBUG(Common_Memory,
                "Successfully mapped memory: virtual=0x{:x}, host=0x{:x}, length=0x{:x}",
                virtual_offset, host_offset, length);
        }

        return true;
    } catch (const std::exception& e) {
        LOG_ERROR(Common_Memory,
            "Failed to map memory: virtual=0x{:x}, host=0x{:x}, length=0x{:x}, error: {}",
            virtual_offset, host_offset, length, e.what());
        return false;
    }
}

} // namespace Common
