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
    enum class ELoadMode : std::uint32_t
    {
        DEFAULT = 0,
        RTLD_LAZY = 1u << 0,
        RTLD_NOW = 1u << 1,
        RTLD_GLOBAL = 1u << 2,
        ALTERED_SEARCH_PATH = 1u << 3,
        APPEND_DECORATIONS = 1u << 4,
        INSTALLED_PLUGIN = 1u << 5,
    };

    constexpr ELoadMode operator|(ELoadMode a, ELoadMode b) noexcept
    {
        using U = std::underlying_type_t<ELoadMode>;
        return static_cast<ELoadMode>(static_cast<U>(a) | static_cast<U>(b));
    }

    constexpr ELoadMode operator&(ELoadMode a, ELoadMode b) noexcept
    {
        using U = std::underlying_type_t<ELoadMode>;
        return static_cast<ELoadMode>(static_cast<U>(a) & static_cast<U>(b));
    }

    constexpr bool any(ELoadMode m) noexcept
    {
        return static_cast<std::underlying_type_t<ELoadMode>>(m) != 0u;
    }
}
