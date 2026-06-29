#pragma once
/**
 * @file DynamicLibrary.hpp
 * @brief Cross-platform RAII wrapper over LoadLibrary / dlopen, with
 *        first-class support for loading from an in-memory image.
 *
 * Path-based loading uses the platform loader directly (`LoadLibraryExW` on
 * Windows, `dlopen` on POSIX).
 *
 * Memory-based loading (`load_from_memory`) abstracts away the per-platform
 * mechanism:
 *  - **Linux**: `memfd_create(2)` + `dlopen("/proc/self/fd/N")`. Truly zero
 *    on-disk footprint. Falls back to `/dev/shm` on kernels < 3.17.
 *  - **Windows**: by default writes the image to a per-process scratch file
 *    in `%TEMP%/lux/<pid>/` opened with FILE_FLAG_DELETE_ON_CLOSE so the OS
 *    reaps it on process exit, then calls `LoadLibraryExW`. When compiled
 *    with `LUX_DYNAMIC_LIBRARY_USE_MEMORY_MODULE=ON`, falls back to the
 *    bundled MemoryModule loader for true zero-disk operation.
 *  - **macOS**: `mkstemp` + `dlopen` + `unlink` (dyld has no public from-memory API).
 *
 * The chosen backend keeps any temporary resources alive via `MemoryBacking`,
 * which is destroyed together with the library handle.
 */

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <lux/engine/dynamic_library/visibility.h>
#include <lux/engine/dynamic_library/LoadMode.hpp>

namespace lux::engine::platform
{
    class LUX_PLATFORM_DYNAMIC_LIBRARY_PUBLIC DynamicLibrary
    {
    public:
        using NativeHandle = void*;

        DynamicLibrary() noexcept;
        ~DynamicLibrary();

        DynamicLibrary(const DynamicLibrary&)            = delete;
        DynamicLibrary& operator=(const DynamicLibrary&) = delete;

        DynamicLibrary(DynamicLibrary&& other) noexcept;
        DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;

        /// Construct + load. Check is_loaded() afterwards.
        explicit DynamicLibrary(const std::filesystem::path& path,
                                LoadMode mode = LoadMode::Default);

        bool load(const std::filesystem::path& path,
                  LoadMode mode = LoadMode::Default);

        /**
         * @brief Load a shared library directly from an in-memory image.
         * @param image     Complete PE / ELF / Mach-O bytes.
         * @param hint_name Used only for diagnostics / temp-file naming.
         * @return true on success; on failure `last_error()` is populated.
         */
        bool load_from_memory(std::span<const std::byte> image,
                              std::string_view hint_name = {});

        void unload() noexcept;

        bool         is_loaded() const noexcept { return handle_ != nullptr; }
        NativeHandle native_handle() const noexcept { return handle_; }

        /// Raw symbol lookup. Returns nullptr if not found.
        void* get_symbol(const char* name) const noexcept;

        template <typename T>
        T* get_symbol(const char* name) const noexcept
        {
            return reinterpret_cast<T*>(get_symbol(name));
        }

        const std::string& last_error() const noexcept { return last_error_; }

        struct MemoryBacking;

    private:
        NativeHandle                   handle_ = nullptr;
        std::string                    last_error_;
        std::unique_ptr<MemoryBacking> backing_;
        bool                           uses_memory_module_ = false;
    };
}
