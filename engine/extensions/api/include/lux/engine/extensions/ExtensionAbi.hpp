#pragma once
/** @file ExtensionAbi.hpp @brief Aggregated same-toolchain Extension ABI v5. */

#include <lux/engine/extensions/ExtensionDescriptor.hpp>
#include <lux/engine/extensions/ExtensionId.hpp>
#include <lux/engine/extensions/ExtensionRegistrarFwd.hpp>
#include <lux/engine/extensions/ExtensionResult.hpp>
#include <lux/engine/extensions/ExtensionVersion.hpp>

namespace lux::extensions
{
    using GetExtensionModuleV5Fn =
        const ExtensionModuleDescriptorV5*() noexcept;
    using InstallWorldSystemsV5Fn =
        ExtensionRegistrationResult(lux::ecs::ScheduleBuilder&) noexcept;
    using InstallRenderFeaturesV5Fn =
        ExtensionRegistrationResult(lux::render::FeatureCatalog&) noexcept;
    using RegisterEditorContributionsV5Fn =
        ExtensionRegistrationResult(EditorContributionRegistrar&) noexcept;

    inline constexpr const char* kGetExtensionModuleV5Symbol =
        "luxGetExtensionModuleV5";
    inline constexpr const char* kInstallWorldSystemsV5Symbol =
        "luxInstallWorldSystemsV5";
    inline constexpr const char* kInstallRenderFeaturesV5Symbol =
        "luxInstallRenderFeaturesV5";
    inline constexpr const char* kRegisterEditorContributionsV5Symbol =
        "luxRegisterEditorContributionsV5";
}
