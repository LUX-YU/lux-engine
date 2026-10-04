#include "Probe.hpp"
#include <lux/engine/editor/extensions/EditorExtension.hpp>

// Only the application qualification uses this second plugin. It observes explicit providers,
// without giving the skeleton editor extra capabilities or exposing an Application implementation.
namespace
{
    skeleton::Facts* observations{};
    lux::editor::extensions::ContributionResult<void> contribute(
        lux::editor::extensions::ContributionDraft&,
        lux::object::CodeLease
    )
    {
        return {};
    }
    lux::editor::extensions::ContributionResult<void> observe(
        lux::editor::extensions::ContributionDraft&,
        lux::object::CodeLease,
        const lux::editor::extensions::ExtensionCapabilities& capabilities
    )
    {
        if (observations)
        {
            observations->project = &capabilities.project->project;
            observations->host = &capabilities.workbench->views;
        }
        return {};
    }
} // namespace
extern "C" SKELETON_EXPORT void ec1_observe(skeleton::Facts* facts) noexcept
{
    observations = facts;
}
extern "C" SKELETON_EXPORT const lux::editor::extensions::EditorExtensionExports* lux_editor_exports_v10() noexcept
{
    using namespace lux::editor::extensions;
    static const EditorExtensionExports exports{
        sizeof(exports),
        kEditorExtensionVersion,
        kEditorExtensionAbi,
        {},
        &contribute,
        {},
        {.project = true, .workbench = true},
        &observe
    };
    return &exports;
}
