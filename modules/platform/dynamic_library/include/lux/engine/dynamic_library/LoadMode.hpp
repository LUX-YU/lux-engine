#pragma once
/**
 * @file LoadMode.hpp
 * @brief Bit-flag enum controlling how a `DynamicLibrary` is opened.
 *
 * Flags map onto the closest equivalent on each platform:
 *  - `RTLD_*` map to dlopen(3) flags on POSIX; ignored on Windows.
 *  - `LoadWithAlteredSearchPath` maps to LOAD_WITH_ALTERED_SEARCH_PATH on
 *    Windows; ignored on POSIX.
 *  - `AppendDecorations` is interpreted by the loader: if set, a bare name
 *    such as `"foo"` is decorated according to the host platform
 *    (`foo.dll` on Windows, `libfoo.so` on Linux, `libfoo.dylib` on macOS).
 */

#include <cstdint>
#include <type_traits>

namespace lux::engine::platform
{
    enum class LoadMode : std::uint32_t
    {
        Default                   = 0,
        RTLD_Lazy                 = 1u << 0,
        RTLD_Now                  = 1u << 1,
        RTLD_Global               = 1u << 2,
        LoadWithAlteredSearchPath = 1u << 3,
        AppendDecorations         = 1u << 4,
    };

    constexpr LoadMode operator|(LoadMode a, LoadMode b) noexcept
    {
        using U = std::underlying_type_t<LoadMode>;
        return static_cast<LoadMode>(static_cast<U>(a) | static_cast<U>(b));
    }

    constexpr LoadMode operator&(LoadMode a, LoadMode b) noexcept
    {
        using U = std::underlying_type_t<LoadMode>;
        return static_cast<LoadMode>(static_cast<U>(a) & static_cast<U>(b));
    }

    constexpr bool any(LoadMode m) noexcept
    {
        return static_cast<std::underlying_type_t<LoadMode>>(m) != 0u;
    }
}
