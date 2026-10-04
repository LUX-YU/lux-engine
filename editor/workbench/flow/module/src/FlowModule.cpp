#include <lux/engine/editor/extensions/EditorExtension.hpp>
#include <lux/engine/editor/flowforge/FlowModule.hpp>
#include <lux/engine/editor/flowforge/FlowSessionFactory.hpp>
#include <lux/engine/editor/flowforge/FlowView.hpp>

namespace lux::editor::flowforge
{
    const extensions::EditorModuleDescriptor& flowModule() noexcept
    {
        static constexpr extensions::EditorModuleDescriptor descriptor{
            "lux.editor.flowforge",
            1,
            +[]() noexcept -> const extensions::EditorExtensionExports*
            {
                static const extensions::EditorExtensionExports exports{
                    .counts = {.commands = 1, .sessions = 1, .services = 2, .ui = 1},
                    .contribute = +[](extensions::ContributionDraft& draft,
                                      object::CodeLease code) -> extensions::ContributionResult<void>
                    {
                        draft.services.push_back(services::ServiceEntry::bind<kFlowEnvironmentService>(code));
                        draft.services.push_back(services::ServiceEntry::bind<kFlowCompilationService>(code));
                        draft.commands.push_back(makeNewFlowCommand(code));
                        draft.sessions.push_back(makeFlowSessionFactory(code));
                        draft.ui.push_back(desktop::UiEntry::bind<kFlowView>(std::move(code)));
                        return {};
                    }
                };
                return &exports;
            }
        };
        return descriptor;
    }
} // namespace lux::editor::flowforge
