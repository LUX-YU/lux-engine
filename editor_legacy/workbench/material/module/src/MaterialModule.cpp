#include <lux/engine/editor/extensions/EditorExtension.hpp>
#include <lux/engine/editor/material/MaterialModule.hpp>
#include <lux/engine/editor/material/MaterialSessionFactory.hpp>
#include <lux/engine/editor/material/MaterialView.hpp>

namespace lux::editor::material
{
    const extensions::EditorModuleDescriptor& materialModule() noexcept
    {
        static constexpr extensions::EditorModuleDescriptor descriptor{
            "lux.editor.material",
            1,
            +[]() noexcept -> const extensions::EditorExtensionExports*
            {
                static const extensions::EditorExtensionExports exports{
                    .counts = {.commands = 1, .sessions = 1, .services = 1, .ui = 1},
                    .contribute = +[](extensions::ContributionDraft& draft,
                                      object::CodeLease code) -> extensions::ContributionResult<void>
                    {
                        draft.services.push_back(services::ServiceEntry::bind<kMaterialCompilationService>(code));
                        draft.commands.push_back(makeNewMaterialCommand(code));
                        draft.sessions.push_back(makeMaterialSessionFactory(code));
                        draft.ui.push_back(desktop::UiEntry::bind<kMaterialView>(std::move(code)));
                        return {};
                    }
                };
                return &exports;
            }
        };
        return descriptor;
    }
} // namespace lux::editor::material
