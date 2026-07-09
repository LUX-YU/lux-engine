#include <lux/engine/dynamic_library/DynamicLibrary.hpp>
#include <lux/engine/dynamic_library/LibraryDecoration.hpp>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <utility>

#include <dlfcn.h>

#if defined(__linux__)
#  include <sys/mman.h>
#  include <sys/syscall.h>
#  ifndef MFD_CLOEXEC
#    define MFD_CLOEXEC 0x0001U
#  endif
#endif

namespace lux::engine::platform
{
    namespace
    {
        int dlopenFlags(LoadMode mode)
        {
            int flags = 0;
            if (any(mode & LoadMode::RTLD_Lazy))   flags |= RTLD_LAZY;
            if (any(mode & LoadMode::RTLD_Now))    flags |= RTLD_NOW;
            if (any(mode & LoadMode::RTLD_Global)) flags |= RTLD_GLOBAL;
            if (flags == 0) flags = RTLD_NOW | RTLD_LOCAL;
            return flags;
        }

#if defined(__linux__)
        // glibc < 2.27 does not wrap memfd_create; fall back to syscall.
        int memfdCreateCompat(const char* name, unsigned int flags)
        {
#  if defined(__GLIBC_PREREQ) && __GLIBC_PREREQ(2, 27)
            return ::memfd_create(name, flags);
#  elif defined(SYS_memfd_create)
            return static_cast<int>(::syscall(SYS_memfd_create, name, flags));
#  else
            (void)name; (void)flags;
            errno = ENOSYS;
            return -1;
#  endif
        }
#endif

        std::string sanitizeHint(std::string_view hint)
        {
            std::string out;
            out.reserve(hint.size() + 16);
            out = "lux_script_";
            for (char c : hint)
            {
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-')
                    out.push_back(c);
            }
            return out;
        }
    }

    // POSIX backend: either a memfd (Linux fast path) or a tmp-file (macOS / fallback).
    struct DynamicLibrary::MemoryBacking
    {
        int         fd        = -1;
        std::string scratch_path;   // non-empty when a real path was used (mkstemp / shm).
        bool        unlinked  = false;

        ~MemoryBacking()
        {
            if (fd >= 0) ::close(fd);
            if (!scratch_path.empty() && !unlinked)
                ::unlink(scratch_path.c_str());
        }
    };

    DynamicLibrary::DynamicLibrary() noexcept = default;
    DynamicLibrary::~DynamicLibrary() { unload(); }

    DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept
        : handle_(other.handle_),
          last_error_(std::move(other.last_error_)),
          backing_(std::move(other.backing_)),
          uses_memory_module_(other.uses_memory_module_)
    {
        other.handle_             = nullptr;
        other.uses_memory_module_ = false;
    }

    DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept
    {
        if (this != &other)
        {
            unload();
            handle_             = other.handle_;
            last_error_         = std::move(other.last_error_);
            backing_            = std::move(other.backing_);
            uses_memory_module_ = other.uses_memory_module_;
            other.handle_             = nullptr;
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

        handle_ = ::dlopen(effective.c_str(), dlopenFlags(mode));
        if (!handle_)
        {
            const char* err = ::dlerror();
            last_error_ = err ? err : "dlopen failed";
        }
        return handle_ != nullptr;
    }

    bool DynamicLibrary::load_from_memory(std::span<const std::byte> image,
                                          std::string_view hint_name)
    {
        unload();
        last_error_.clear();

        if (image.empty())
        {
            last_error_ = "empty image";
            return false;
        }

        const std::string sanitized = sanitizeHint(hint_name);

#if defined(__linux__)
        // Preferred path: anonymous memory file descriptor + dlopen via /proc.
        // Extra braces scope this fd so it does not clash with the fallback fd below
        // (both paths are compiled on Linux, unlike Windows where only one is).
        {
        int fd = memfdCreateCompat(sanitized.c_str(), MFD_CLOEXEC);
        if (fd >= 0)
        {
            const std::byte* p = image.data();
            std::size_t      n = image.size();
            while (n > 0)
            {
                const ssize_t w = ::write(fd, p, n);
                if (w <= 0)
                {
                    if (errno == EINTR) continue;
                    last_error_ = std::strerror(errno);
                    ::close(fd);
                    return false;
                }
                p += w;
                n -= static_cast<std::size_t>(w);
            }

            char proc_path[64];
            std::snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);

            void* h = ::dlopen(proc_path, RTLD_NOW | RTLD_LOCAL);
            if (!h)
            {
                const char* err = ::dlerror();
                last_error_ = err ? err : "dlopen(/proc/self/fd) failed";
                ::close(fd);
                return false;
            }

            auto backing = std::make_unique<MemoryBacking>();
            backing->fd = fd;   // keep alive; dlclose will release the mapping.
            backing_    = std::move(backing);
            handle_     = h;
            return true;
        }
        }   // end memfd scope
        // memfd_create failed (old kernel, sandbox, etc.) → fall through to tmp file.
#endif

        // Fallback / macOS path: write to a temp file, dlopen, then unlink so
        // the directory entry disappears even though dyld keeps the mapping.
        const char* tmpdir = std::getenv("TMPDIR");
        if (!tmpdir || !*tmpdir) tmpdir = "/tmp";

        std::string tmpl;
        tmpl.reserve(std::strlen(tmpdir) + sanitized.size() + 32);
        tmpl  = tmpdir;
        tmpl += '/';
        tmpl += sanitized;
        tmpl += "_XXXXXX";
#if defined(__APPLE__)
        tmpl += ".dylib";
        const int fd = ::mkstemps(&tmpl[0], 6);   // 6 = strlen(".dylib")
#else
        tmpl += ".so";
        const int fd = ::mkstemps(&tmpl[0], 3);   // 3 = strlen(".so")
#endif
        if (fd < 0)
        {
            last_error_ = std::strerror(errno);
            return false;
        }

        const std::byte* p = image.data();
        std::size_t      n = image.size();
        while (n > 0)
        {
            const ssize_t w = ::write(fd, p, n);
            if (w <= 0)
            {
                if (errno == EINTR) continue;
                last_error_ = std::strerror(errno);
                ::close(fd);
                ::unlink(tmpl.c_str());
                return false;
            }
            p += w;
            n -= static_cast<std::size_t>(w);
        }

        void* h = ::dlopen(tmpl.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h)
        {
            const char* err = ::dlerror();
            last_error_ = err ? err : "dlopen failed";
            ::close(fd);
            ::unlink(tmpl.c_str());
            return false;
        }

        // Unlink immediately: the loader has already mmap'd the file.
        ::unlink(tmpl.c_str());
        auto backing = std::make_unique<MemoryBacking>();
        backing->fd           = fd;
        backing->scratch_path = std::move(tmpl);
        backing->unlinked     = true;
        backing_              = std::move(backing);
        handle_               = h;
        return true;
    }

    void DynamicLibrary::unload() noexcept
    {
        if (handle_)
        {
            ::dlclose(handle_);
            handle_ = nullptr;
        }
        backing_.reset();
    }

    void* DynamicLibrary::get_symbol(const char* name) const noexcept
    {
        if (!handle_ || !name) return nullptr;
        return ::dlsym(handle_, name);
    }

    std::filesystem::path decorate(std::string_view base)
    {
        std::filesystem::path p(std::string{base});
        std::string filename = p.filename().string();
        if (filename.rfind("lib", 0) != 0)
            filename = "lib" + filename;
#if defined(__APPLE__)
        if (p.extension().string() != ".dylib")
            filename += ".dylib";
#else
        if (p.extension().string() != ".so")
            filename += ".so";
#endif
        return p.parent_path() / filename;
    }

    std::string undecorate(const std::filesystem::path& path)
    {
        std::string stem = path.stem().string();
        if (stem.rfind("lib", 0) == 0)
            stem.erase(0, 3);
        return stem;
    }
}
