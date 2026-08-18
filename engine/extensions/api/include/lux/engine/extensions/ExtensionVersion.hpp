#pragma once

#include <lux/cxx/abi/Abi.hpp>

#include <cstdint>

namespace lux::extensions
{
    using ExtensionVersion = lux::cxx::SemanticVersion;

    [[nodiscard]] constexpr bool satisfiesExtensionVersion(
        const ExtensionVersion& version,
        std::uint16_t required_major,
        std::uint16_t minimum_minor) noexcept
    {
        return version.major == required_major && version.minor >= minimum_minor;
    }
}
