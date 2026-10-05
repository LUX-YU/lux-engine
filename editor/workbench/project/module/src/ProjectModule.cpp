#include <lux/engine/editor/extensions/EditorExtension.hpp>
#include <lux/engine/editor/assets/ModelImporter.hpp>
#include <lux/engine/editor/persistence/PersistenceServices.hpp>
#include <lux/engine/editor/project/ContentReview.hpp>
#include <lux/engine/editor/launcher/LaunchEditor.hpp>
#include <lux/engine/editor/project/ContentViews.hpp>
#include <lux/engine/editor/project/ProjectCreationView.hpp>
#include <lux/engine/editor/project/ProjectModule.hpp>
#include <lux/engine/editor/project/ProjectView.hpp>
#include <lux/engine/editor/scene/ProjectSceneEnvironment.hpp>
#include <lux/engine/editor/sessions/SessionServices.hpp>
#include <lux/engine/editor/sessions/SessionOpening.hpp>
#include <lux/engine/editor/storage/ProjectContentReloading.hpp>
#include <lux/engine/editor/storage/ProjectContentSaving.hpp>
#include <lux/engine/editor/storage/ProjectPluginSelection.hpp>
#include <lux/engine/editor/storage/PublicationFileStore.hpp>
#include <lux/engine/editor/storage/RecentProjects.hpp>

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
                    .counts = {.commands = 2, .services = 16, .ui = 2},
                    .contribute = +[](extensions::ContributionDraft& draft,
                                      object::CodeLease code) -> extensions::ContributionResult<void>
                    {
                        auto history = sessions::makeHistoryCommands(code);
                        draft.commands.insert(draft.commands.end(), history.begin(), history.end());
                        draft.services.push_back(services::ServiceEntry::bind<sessions::kSessionStoreService>(code));
                        draft.services.push_back(services::ServiceEntry::bind<sessions::kSessionOpeningService>(code));
                        draft.services.push_back(
                            services::ServiceEntry::bind<persistence::kWriteCoordinatorService>(code)
                        );
                        draft.services.push_back(services::ServiceEntry::bind<persistence::kSaveService>(code));
                        draft.services.push_back(services::ServiceEntry::bind<persistence::kSaveExecutionService>(code)
                        );
                        draft.services.push_back(
                            services::ServiceEntry::bind<storage::kPublicationFileStoreService>(code)
                        );
                        draft.services.push_back(services::ServiceEntry::bind<kProjectContentSavingService>(code));
                        draft.services.push_back(services::ServiceEntry::bind<kProjectContentReloadingService>(code));
                        draft.services.push_back(services::ServiceEntry::bind<kProjectPluginSelectionService>(code));
                        draft.services.push_back(services::ServiceEntry::bind<scene::kProjectSceneEnvironment>(code));
                        draft.services.push_back(services::ServiceEntry::bind<kContentReviewService>(code));
                        draft.services.push_back(services::ServiceEntry::bind<ContentViews::service>(code));
                        draft.services.push_back(services::ServiceEntry::bind<kProjectCreationService>(code));
                        draft.services.push_back(services::ServiceEntry::bind<kProjectLaunchingService>(code));
                        draft.services.push_back(services::ServiceEntry::bind<assets::kModelImporterService>(code));
                        draft.services.push_back(services::ServiceEntry::bind<kRecentProjectsService>(code));
                        draft.ui.push_back(desktop::UiEntry::bind<kProjectCreationView>(code));
                        draft.ui.push_back(desktop::UiEntry::bind<kProjectView>(std::move(code)));
                        return {};
                    }
                };
                return &exports;
            }
        };
        return descriptor;
    }
} // namespace lux::editor::project
