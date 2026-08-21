#pragma once
/** @file ExtensionDescriptor.hpp @brief Frozen Extension ABI v4 descriptor. */

#include <lux/engine/extensions/ExtensionVersion.hpp>

#include <lux/cxx/abi/Abi.hpp>
#include <lux/cxx/abi/BuildInfo.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace lux::extensions
{
    inline constexpr std::uint32_t kExtensionAbiV4 = 4u;
    inline constexpr auto kEngineExtensionAbiFingerprint =
        lux::cxx::AbiBuildInfo::fingerprint();

    enum class EExtensionModuleTarget : std::uint8_t
    {
        RUNTIME = 0u,
        EDITOR = 1u
    };

    struct ExtensionDependencyView final
    {
        lux::cxx::AbiStringView id;
        std::uint16_t required_major{0u};
        std::uint16_t minimum_minor{0u};
    };

    struct ExtensionModuleDescriptorV4 final
    {
        std::uint32_t struct_size{sizeof(ExtensionModuleDescriptorV4)};
        std::uint32_t extension_abi{kExtensionAbiV4};
        lux::cxx::AbiFingerprint engine_abi_fingerprint{
            kEngineExtensionAbiFingerprint};
        lux::cxx::AbiStringView id;
        ExtensionVersion version;
        EExtensionModuleTarget target{EExtensionModuleTarget::RUNTIME};
        const ExtensionDependencyView* dependencies{nullptr};
        std::size_t dependency_count{0u};
    };

    static_assert(std::is_standard_layout_v<ExtensionDependencyView>);
    static_assert(std::is_standard_layout_v<ExtensionModuleDescriptorV4>);
}
