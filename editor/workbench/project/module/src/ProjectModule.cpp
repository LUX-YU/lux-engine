#include <lux/engine/editor/extensions/EditorExtension.hpp>
#include <lux/engine/editor/project/ProjectModule.hpp>
#include <lux/engine/editor/project/ProjectView.hpp>
#include <lux/engine/editor/scene/ProjectSceneEnvironment.hpp>
#include <lux/engine/editor/storage/ProjectContentSaving.hpp>

namespace lux::editor::project
{
    const extensions::EditorModuleDescriptor& projectModule() noexcept
    {
        static constexpr extensions::EditorModuleDescriptor descriptor{
            "lux.editor.project",
            1,
            +[]() noexcept -> const extensions::EditorExtensionExports*
            {
                static const extensions::EditorExtensionExports exports{
                    .counts = {.services = 2, .ui = 1},
                    .contribute = +[](extensions::ContributionDraft& draft,
                                      object::CodeLease code) -> extensions::ContributionResult<void>
                    {
                        draft.services.push_back(services::ServiceEntry::bind<kProjectContentSavingService>(code));
                        draft.services.push_back(services::ServiceEntry::bind<scene::kProjectSceneEnvironment>(code));
                        draft.ui.push_back(desktop::UiEntry::bind<kProjectView>(std::move(code)));
                        return {};
                    }
                };
                return &exports;
            }
        };
        return descriptor;
    }
}
