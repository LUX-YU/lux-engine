#include <lux/engine/editor/extensions/EditorExtension.hpp>
#include <lux/engine/editor/scene/ModelPlacementService.hpp>
#include <lux/engine/editor/scene/RunStore.hpp>
#include <lux/engine/editor/scene/SceneCreationView.hpp>
#include <lux/engine/editor/scene/SceneModule.hpp>
#include <lux/engine/editor/scene/ScenePlayback.hpp>
#include <lux/engine/editor/scene/SceneSessionFactory.hpp>
#include <lux/engine/editor/scene/SceneTools.hpp>
#include <lux/engine/editor/scene/SceneView.hpp>

namespace lux::editor::scene
{
    const extensions::EditorModuleDescriptor& sceneModule() noexcept
    {
        static constexpr extensions::EditorModuleDescriptor descriptor{
            "lux.editor.scene",
            1,
            +[]() noexcept -> const extensions::EditorExtensionExports*
            {
                static const extensions::EditorExtensionExports exports{
                    .counts = {.sessions = 1, .services = 4, .ui = 7},
                    .contribute = +[](extensions::ContributionDraft& draft,
                                      object::CodeLease code) -> extensions::ContributionResult<void>
                    {
                        draft.services.push_back(services::ServiceEntry::bind<kRunStoreService>(code));
                        draft.services.push_back(services::ServiceEntry::bind<kScenePlaybackService>(code));
                        draft.services.push_back(services::ServiceEntry::bind<kScenePresentationHub>(code));
                        draft.services.push_back(services::ServiceEntry::bind<kModelPlacementService>(code));
                        draft.sessions.push_back(makeSceneSessionFactory(code));
                        draft.ui.push_back(desktop::UiEntry::bind<kSceneView>(code));
                        draft.ui.push_back(desktop::UiEntry::bind<kSceneCreationView>(code));
                        draft.ui.push_back(desktop::UiEntry::bind<kOutlinerView>(code));
                        draft.ui.push_back(desktop::UiEntry::bind<kInspectorView>(code));
                        draft.ui.push_back(desktop::UiEntry::bind<kRunInspectorView>(code));
                        draft.ui.push_back(desktop::UiEntry::bind<kResourceView>(code));
                        draft.ui.push_back(desktop::UiEntry::bind<kSceneConfigurationView>(std::move(code)));
                        return {};
                    }
                };
                return &exports;
            }
        };
        return descriptor;
    }
} // namespace lux::editor::scene
