#include <lux/engine/editor/ui/scene/SceneCreationPane.hpp>
#include <lux/engine/editor/ui/SceneConfigurationElement.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <lux/engine/project/PluginManager.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>

namespace lux::editor::ui
{
    namespace
    {
        constexpr SceneProviderOption providers[]{
            {"lux.render.runtime", "main-window"},
            {"lux.render.scene_bindings", "render-bindings"},
            {"lux.render.resources", "resources"},
            {"lux.render.assets", "assets"},
            {"lux.world.loading", "world-storage"}
        };
        class CreationPane final : public lux::ui::Pane
        {
        public:
            CreationPane(scene::SceneEditor& editor, EditorContext& context, EditorResult<void>& status)
                : Pane(
                      editor,
                      lux::ui::PaneId{std::string(editor.id().name()) + "/new"},
                      lux::ui::PaneTypeId{"lux.editor.scene.creation"},
                      "Create Scene"
                  ),
                  editor_(editor), layout_(*this, lux::ui::ElementId{"content"}),
                  configuration_(
                      layout_,
                      lux::ui::ElementId{"configuration"},
                      context.plugins().catalog(),
                      context.sceneRegistrations(),
                      context.configurationEditors(),
                      providers,
                      status
                  ),
                  actions_(layout_, lux::ui::ElementId{"actions"}, lux::ui::ELayoutType::HORIZONTAL),
                  create_(actions_, lux::ui::ElementId{"create"}, "Create"),
                  cancel_(actions_, lux::ui::ElementId{"cancel"}, "Cancel"),
                  error_(layout_, lux::ui::ElementId{"error"})
            {
                setContent(layout_);
                setModal(true);
                connections_[0] = detail::takeConnection(
                    connect(&create_, &lux::ui::Button::activated, [this]() noexcept { create_requested_ = true; }),
                    status
                );
                connections_[1] = detail::takeConnection(
                    connect(&cancel_, &lux::ui::Button::activated, [this]() noexcept { cancel_requested_ = true; }),
                    status
                );
                connections_[2] = detail::takeConnection(
                    connect(this, &lux::ui::Pane::closeRequested, [this]() noexcept { cancel_requested_ = true; }),
                    status
                );
            }

        private:
            void update() noexcept override
            {
                if (const auto& failure = editor_.assetStatus().failure)
                    error_.setText(failure->domain + ": " + failure->message);
                if (std::exchange(cancel_requested_, false))
                {
                    editor_.cancelNewAsset();
                    return;
                }
                if (!std::exchange(create_requested_, false))
                    return;
                auto selected = configuration_.build();
                EditorResult<void> result;
                if (!selected)
                    result = lux::cxx::unexpected(selected.error());
                else
                    result = editor_.createAsset(
                        selected->name,
                        selected->schemas,
                        selected->simulation,
                        selected->scene,
                        selected->viewport
                    );
                if (!result)
                    error_.setText(result.error().domain + ": " + result.error().message);
            }
            scene::SceneEditor& editor_;
            lux::ui::Layout layout_;
            SceneConfigurationElement configuration_;
            lux::ui::Layout actions_;
            lux::ui::Button create_, cancel_;
            lux::ui::Label error_;
            std::array<object::Connection, 3> connections_;
            bool create_requested_{}, cancel_requested_{};
        };
    }
    EditorResult<std::unique_ptr<lux::ui::Pane>> createSceneCreationPane(
        scene::SceneEditor& editor,
        EditorContext& context
    )
    {
        EditorResult<void> status;
        auto result = std::make_unique<CreationPane>(editor, context, status);
        if (!status)
            return lux::cxx::unexpected(status.error());
        return result;
    }
}
