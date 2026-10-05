#include <lux/engine/editor/extensions/EditorExtension.hpp>
#include <lux/engine/editor/scene/SceneCreationView.hpp>
#include <lux/engine/editor/scene/SceneModule.hpp>
#include <lux/engine/editor/scene/SceneSessionFactory.hpp>
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
                    .counts = {.sessions = 1, .services = 1, .ui = 2},
                    .contribute = +[](extensions::ContributionDraft& draft,
                                      object::CodeLease code) -> extensions::ContributionResult<void>
                    {
                        draft.services.push_back(services::ServiceEntry::bind<kScenePresentationHub>(code));
                        draft.sessions.push_back(makeSceneSessionFactory(code));
                        draft.ui.push_back(desktop::UiEntry::bind<kSceneView>(code));
                        draft.ui.push_back(desktop::UiEntry::bind<kSceneCreationView>(std::move(code)));
                        return {};
                    }
                };
                return &exports;
            }
        };
        return descriptor;
    }
} // namespace lux::editor::scene
