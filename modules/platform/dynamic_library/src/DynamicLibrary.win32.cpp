#include <lux/engine/dynamic_library/DynamicLibrary.hpp>
#include <lux/engine/dynamic_library/LibraryDecoration.hpp>

#include <atomic>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#if defined(LUX_DYNAMIC_LIBRARY_USE_MEMORY_MODULE)
#include "MemoryModule.h" // bundled third-party (modules/platform/dynamic_library/third_party/MemoryModule)
#endif

namespace lux::engine::platform
{
    namespace
    {
        std::string lastWin32Error()
        {
            const DWORD err = ::GetLastError();
            if (err == 0)
                return {};

            LPSTR buf = nullptr;
            const DWORD len = ::FormatMessageA(
                FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                nullptr,
                err,
                MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                reinterpret_cast<LPSTR>(&buf),
                0,
                nullptr
            );

            std::string msg = (len > 0 && buf != nullptr) ? std::string(buf, len) : std::string{};
            if (buf)
                ::LocalFree(buf);

            while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
                msg.pop_back();
            return msg;
        }

        DWORD toWin32Flags(LoadMode mode)
        {
            DWORD flags = 0;
            if (any(mode & LoadMode::LoadWithAlteredSearchPath))
                flags |= LOAD_WITH_ALTERED_SEARCH_PATH;
            return flags;
        }

        /// Reap scratch dirs left behind by processes that died before their
        /// DynamicLibrary destructors ran (crash / kill). Scratch files are
        /// deleted explicitly on unload — DELETE_ON_CLOSE is NOT an option
        /// here (see load_from_memory) — so a crashed process leaks its
        /// %TEMP%/lux/<pid>/ dir; this sweep collects them on the next run.
        void sweepStaleScratchDirs(const std::filesystem::path& lux_root)
        {
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(lux_root, ec))
            {
                if (!entry.is_directory(ec))
                    continue;

                const std::wstring name = entry.path().filename().wstring();
                DWORD pid = 0;
                for (const wchar_t c : name)
                {
                    if (c < L'0' || c > L'9')
                    {
                        pid = 0;
                        break;
                    }
                    pid = pid * 10u + static_cast<DWORD>(c - L'0');
                }
                if (pid == 0 || pid == ::GetCurrentProcessId())
                    continue;

                bool alive = false;
                if (HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
                {
                    DWORD code = 0;
                    alive = ::GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
                    ::CloseHandle(h);
                }
                else
                {
                    // Unopenable: ACCESS_DENIED means it exists but is
                    // shielded — leave it alone. Anything else = dead pid.
                    alive = ::GetLastError() == ERROR_ACCESS_DENIED;
                }
                if (!alive)
                    std::filesystem::remove_all(entry.path(), ec);
            }
        }

        std::filesystem::path makeScratchPath(std::string_view hint)
        {
            wchar_t temp_dir[MAX_PATH];
            const DWORD n = ::GetTempPathW(MAX_PATH, temp_dir);
            std::filesystem::path base =
                (n > 0) ? std::filesystem::path(std::wstring(temp_dir, n)) : std::filesystem::path(L".");
            base /= L"lux";

            static std::once_flag s_sweep_once;
            std::call_once(s_sweep_once, [&base] { sweepStaleScratchDirs(base); });

            base /= std::to_wstring(::GetCurrentProcessId());
            std::error_code ec;
            std::filesystem::create_directories(base, ec);

            std::wstring stem = L"lux_script_";
            for (char c : hint)
            {
                const bool is_lowercase = c >= 'a' && c <= 'z';
                const bool is_uppercase = c >= 'A' && c <= 'Z';
                const bool is_digit = c >= '0' && c <= '9';
                const bool is_allowed_symbol = c == '_' || c == '-';
                const bool is_valid_character = is_lowercase || is_uppercase || is_digit || is_allowed_symbol;
                if (is_valid_character)
                    stem.push_back(static_cast<wchar_t>(c));
            }
            if (stem == L"lux_script_")
                stem = L"lux_script";

            // Suffix with a counter so multiple loads in one process do not collide.
            static std::atomic<unsigned> s_seq{0u};
            stem += L"_";
            stem += std::to_wstring(s_seq.fetch_add(1u, std::memory_order_relaxed));
            stem += L".dll";

            return base / stem;
        }
    }

    struct DynamicLibrary::MemoryBacking
    {
        // Either a path (default Windows path) or a MemoryModule handle.
        std::filesystem::path scratch_path;
#if defined(LUX_DYNAMIC_LIBRARY_USE_MEMORY_MODULE)
        HMEMORYMODULE mm_handle = nullptr;
#endif

        ~MemoryBacking()
        {
            // Runs AFTER unload() called FreeLibrary — the image mapping is
            // gone, so the delete succeeds. Crash-before-dtor leaks the file;
            // sweepStaleScratchDirs reaps it on the next run.
            if (!scratch_path.empty())
            {
                std::error_code ec;
                std::filesystem::remove(scratch_path, ec);
            }
        }
    };

    DynamicLibrary::DynamicLibrary() noexcept = default;
    DynamicLibrary::~DynamicLibrary()
    {
        unload();
    }

    DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept
        : handle_(other.handle_), last_error_(std::move(other.last_error_)), backing_(std::move(other.backing_)),
          uses_memory_module_(other.uses_memory_module_)
    {
        other.handle_ = nullptr;
        other.uses_memory_module_ = false;
    }

    DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept
    {
        if (this != &other)
        {
            unload();
            handle_ = other.handle_;
            last_error_ = std::move(other.last_error_);
            backing_ = std::move(other.backing_);
            uses_memory_module_ = other.uses_memory_module_;
            other.handle_ = nullptr;
            other.uses_memory_module_ = false;
        }
        return *this;
    }

    DynamicLibrary::DynamicLibrary(const std::filesystem::path& path, LoadMode mode)
    {
        load(path, mode);
    }

    bool DynamicLibrary::load(const std::filesystem::path& path, LoadMode mode)
    {
        unload();
        last_error_.clear();

        std::filesystem::path effective = path;
        if (any(mode & LoadMode::AppendDecorations) && !path.has_extension())
            effective = decorate(path.string());

        handle_ = ::LoadLibraryExW(effective.wstring().c_str(), nullptr, toWin32Flags(mode));
        if (!handle_)
            last_error_ = lastWin32Error();
        return handle_ != nullptr;
    }

    bool DynamicLibrary::load_from_memory(std::span<const std::byte> image, std::string_view hint_name)
    {
        unload();
        last_error_.clear();

        if (image.empty())
        {
            last_error_ = "empty image";
            return false;
        }

#if defined(LUX_DYNAMIC_LIBRARY_USE_MEMORY_MODULE)
        // True zero-disk path: hand the bytes to MemoryModule's PE loader.
        HMEMORYMODULE mm = ::MemoryLoadLibrary(image.data(), image.size());
        if (!mm)
        {
            last_error_ = lastWin32Error();
            if (last_error_.empty())
                last_error_ = "MemoryLoadLibrary failed";
            return false;
        }
        backing_ = std::make_unique<MemoryBacking>();
        backing_->mm_handle = mm;
        handle_ = reinterpret_cast<NativeHandle>(mm);
        uses_memory_module_ = true;
        return true;
#else
        // Default Windows path: write the image to a per-process scratch
        // file, CLOSE the writer, then LoadLibraryExW it.
        //
        // The writer handle must not stay open across the load: the loader
        // opens the image without FILE_SHARE_WRITE, so a live GENERIC_WRITE
        // handle turns every load into ERROR_SHARING_VIOLATION. And
        // FILE_FLAG_DELETE_ON_CLOSE is no escape hatch — its delete
        // disposition arms at that handle's cleanup, which makes the
        // loader's subsequent open fail as delete-pending. Cleanup is
        // explicit instead: unload() removes the file after FreeLibrary,
        // and makeScratchPath sweeps dead processes' scratch dirs once per
        // run (crash-leak recovery).
        auto path = makeScratchPath(hint_name);

        HANDLE fh = ::CreateFileW(
            path.wstring().c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_TEMPORARY,
            nullptr
        );
        if (fh == INVALID_HANDLE_VALUE)
        {
            last_error_ = lastWin32Error();
            return false;
        }

        // Write the whole image; chunk to avoid > 4 GiB single-call limit.
        const std::byte* p = image.data();
        std::size_t remaining = image.size();
        while (remaining > 0)
        {
            const DWORD chunk = static_cast<DWORD>(remaining > 0x4000'0000ull ? 0x4000'0000ull : remaining);
            DWORD written = 0;
            if (!::WriteFile(fh, p, chunk, &written, nullptr) || written != chunk)
            {
                last_error_ = lastWin32Error();
                ::CloseHandle(fh);
                std::error_code ec;
                std::filesystem::remove(path, ec);
                return false;
            }
            p += written;
            remaining -= written;
        }
        ::FlushFileBuffers(fh);
        ::CloseHandle(fh); // release WRITE access BEFORE the loader opens it

        handle_ = ::LoadLibraryExW(path.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!handle_)
        {
            last_error_ = lastWin32Error();
            std::error_code ec;
            std::filesystem::remove(path, ec);
            return false;
        }

        backing_ = std::make_unique<MemoryBacking>();
        backing_->scratch_path = std::move(path);
        return true;
#endif
    }

    void DynamicLibrary::unload() noexcept
    {
        if (handle_)
        {
#if defined(LUX_DYNAMIC_LIBRARY_USE_MEMORY_MODULE)
            if (uses_memory_module_)
                ::MemoryFreeLibrary(reinterpret_cast<HMEMORYMODULE>(handle_));
            else
                ::FreeLibrary(reinterpret_cast<HMODULE>(handle_));
#else
            ::FreeLibrary(reinterpret_cast<HMODULE>(handle_));
#endif
            handle_ = nullptr;
            uses_memory_module_ = false;
        }
        backing_.reset();
    }

    void* DynamicLibrary::get_symbol(const char* name) const noexcept
    {
        if (!handle_ || !name)
            return nullptr;
#if defined(LUX_DYNAMIC_LIBRARY_USE_MEMORY_MODULE)
        if (uses_memory_module_)
        {
            return reinterpret_cast<void*>(::MemoryGetProcAddress(reinterpret_cast<HMEMORYMODULE>(handle_), name));
        }
#endif
        return reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(handle_), name));
    }

    std::filesystem::path decorate(std::string_view base)
    {
        std::filesystem::path p(base);
        if (!p.has_extension())
            p += L".dll";
        return p;
    }

    std::string undecorate(const std::filesystem::path& path)
    {
        std::string stem = path.stem().string();
        return stem;
    }
}
