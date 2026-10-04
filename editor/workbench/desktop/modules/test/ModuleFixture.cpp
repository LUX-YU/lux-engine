#include "ModuleFixture.hpp"
#include "CompositionProbe.hpp"
#include <lux/engine/editor/extensions/EditorExtension.hpp>

extern "C" void
compositionDefinitions(lux::object::CodeLease, std::shared_ptr<const lux::services::ServiceEntry>&, std::vector<std::shared_ptr<const lux::editor::desktop::UiEntry>>&);

namespace module_fixture
{
    const lux::editor::extensions::EditorModuleDescriptor& module() noexcept
    {
        using namespace lux::editor::extensions;
        static constexpr EditorModuleDescriptor descriptor{
            "ec4.module.fixture",
            1,
            +[]() noexcept -> const EditorExtensionExports*
            {
                static const EditorExtensionExports exports{
                    .counts = {.services = 1, .ui = 2},
                    .contribute = +[](ContributionDraft& draft, lux::object::CodeLease code) -> ContributionResult<void>
                    {
                        std::shared_ptr<const lux::services::ServiceEntry> entry;
                        compositionDefinitions(std::move(code), entry, draft.ui);
                        draft.services.push_back(std::move(entry));
                        return {};
                    }
                };
                return &exports;
            }
        };
        return descriptor;
    }
} // namespace module_fixture
