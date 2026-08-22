#pragma once
/** @file ExtensionAbi.hpp @brief Aggregated Lux Engine Extension ABI v4. */

#include <lux/engine/extensions/ExtensionDescriptor.hpp>
#include <lux/engine/extensions/ExtensionId.hpp>
#include <lux/engine/extensions/ExtensionRegistrarFwd.hpp>
#include <lux/engine/extensions/ExtensionResult.hpp>
#include <lux/engine/extensions/ExtensionVersion.hpp>

namespace lux::extensions
{
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
