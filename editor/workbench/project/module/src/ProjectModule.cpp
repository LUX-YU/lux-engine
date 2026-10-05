#include <lux/engine/editor/extensions/EditorExtension.hpp>
#include <lux/engine/editor/persistence/PersistenceServices.hpp>
#include <lux/engine/editor/project/ProjectModule.hpp>
#include <lux/engine/editor/project/ProjectView.hpp>
#include <lux/engine/editor/scene/ProjectSceneEnvironment.hpp>
#include <lux/engine/editor/sessions/SessionServices.hpp>
#include <lux/engine/editor/storage/ProjectContentSaving.hpp>
#include <lux/engine/editor/storage/ProjectContentReloading.hpp>
#include <lux/engine/editor/storage/ProjectPluginSelection.hpp>
#include <lux/engine/editor/storage/PublicationFileStore.hpp>

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
                    .counts = {.services = 10, .ui = 1},
                    .contribute = +[](extensions::ContributionDraft& draft,
                                      object::CodeLease code) -> extensions::ContributionResult<void>
                    {
                        draft.services.push_back(services::ServiceEntry::bind<sessions::kSessionStoreService>(code));
                        draft.services.push_back(services::ServiceEntry::bind<sessions::kSessionOpeningService>(code));
                        draft.services.push_back(
                            services::ServiceEntry::bind<persistence::kWriteCoordinatorService>(code)
                        );
                        draft.services.push_back(services::ServiceEntry::bind<persistence::kSaveService>(code));
                        draft.services.push_back(
                            services::ServiceEntry::bind<persistence::kSaveExecutionService>(code)
                        );
                        draft.services.push_back(
                            services::ServiceEntry::bind<storage::kPublicationFileStoreService>(code)
                        );
                        draft.services.push_back(services::ServiceEntry::bind<kProjectContentSavingService>(code));
                        draft.services.push_back(services::ServiceEntry::bind<kProjectContentReloadingService>(code));
                        draft.services.push_back(services::ServiceEntry::bind<kProjectPluginSelectionService>(code));
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
