#pragma once
#include <filesystem>
namespace lux::editor::storage::detail
{
#if defined(_WIN32)
    // Keep coordinator keys canonical and platform-neutral. Only the OS/CRT IO boundary
    // uses extended paths, including the staging suffix of immutable model generations.
    inline std::filesystem::path nativePublicationPath(const std::filesystem::path& path)
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
    inline const std::filesystem::path& nativePublicationPath(const std::filesystem::path& path) noexcept
    {
        return path;
    }
#endif
}
