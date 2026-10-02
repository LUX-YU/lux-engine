#pragma once
#include <filesystem>
namespace lux::engine::platform
{
#if defined(_WIN32)
    // Use only at OS/CRT file boundaries, after logical path and confinement validation.
    // Extended Windows paths must not become persisted asset names or coordinator keys.
    inline std::filesystem::path nativeFilePath(const std::filesystem::path& path)
    {
        if (!path.is_absolute())
            return path;
        auto native = path.lexically_normal().make_preferred().native();
        if (native.starts_with(L"\\\\?\\") || native.starts_with(L"\\\\.\\"))
            return native;
        if (native.starts_with(L"\\\\"))
            return L"\\\\?\\UNC\\" + native.substr(2);
        return L"\\\\?\\" + native;
    }
#else
    inline const std::filesystem::path& nativeFilePath(const std::filesystem::path& path) noexcept
    {
        return path;
    }
#endif
}
