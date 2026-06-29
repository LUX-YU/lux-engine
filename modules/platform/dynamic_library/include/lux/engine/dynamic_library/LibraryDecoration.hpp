#pragma once
/**
 * @file LibraryDecoration.hpp
 * @brief Platform-dependent shared-library naming helpers.
 *
 * `decorate("foo")` returns `foo.dll` on Windows, `libfoo.so` on Linux,
 * `libfoo.dylib` on macOS. `undecorate()` is the inverse for files that
 * follow the platform convention; otherwise it returns the stem unchanged.
 */

#include <filesystem>
#include <string>
#include <string_view>

#include <lux/engine/dynamic_library/visibility.h>

namespace lux::engine::platform
{
    LUX_PLATFORM_DYNAMIC_LIBRARY_PUBLIC
    std::filesystem::path decorate(std::string_view base);

    LUX_PLATFORM_DYNAMIC_LIBRARY_PUBLIC
    std::string undecorate(const std::filesystem::path& path);
}
