#pragma once

#include <lux/engine/core/extension_abi/StableId.hpp>

#include <lux/cxx/abi/Abi.hpp>
#include <lux/cxx/abi/BuildInfo.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace lux::extensions
{
    class RuntimeContributionRegistrar;
    class EditorContributionRegistrar;

    inline constexpr std::uint32_t kExtensionAbiV4 = 4u;
    inline constexpr auto kEngineExtensionAbiFingerprint =
        lux::cxx::AbiBuildInfo::fingerprint();

    using ExtensionVersion = lux::cxx::SemanticVersion;

    [[nodiscard]] constexpr bool satisfiesExtensionVersion(
        const ExtensionVersion& version,
        std::uint16_t required_major,
        std::uint16_t minimum_minor) noexcept
    {
        return version.major == required_major &&
            version.minor >= minimum_minor;
    }

    enum class EExtensionModuleTarget : std::uint8_t
    {
        RUNTIME,
        EDITOR
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

    enum class EExtensionRegistrationError : std::uint8_t
    {
        NONE,
        INVALID_DESCRIPTOR,
        DUPLICATE_CONTRIBUTION,
        DUPLICATE_COMPONENT,
        HASH_COLLISION,
        MISSING_DEPENDENCY,
        INVALID_CONFIG,
        INTERNAL_FAILURE
    };

    struct ExtensionRegistrationResult final
    {
        EExtensionRegistrationError error{EExtensionRegistrationError::NONE};

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return error == EExtensionRegistrationError::NONE;
        }
    };

    using GetExtensionModuleV4Fn =
        const ExtensionModuleDescriptorV4*() noexcept;
    using RegisterRuntimeContributionsV4Fn =
        ExtensionRegistrationResult(RuntimeContributionRegistrar&) noexcept;
    using RegisterEditorContributionsV4Fn =
        ExtensionRegistrationResult(EditorContributionRegistrar&) noexcept;

    inline constexpr const char* kGetExtensionModuleV4Symbol =
        "luxGetExtensionModuleV4";
    inline constexpr const char* kRegisterRuntimeContributionsV4Symbol =
        "luxRegisterRuntimeContributionsV4";
    inline constexpr const char* kRegisterEditorContributionsV4Symbol =
        "luxRegisterEditorContributionsV4";
}
