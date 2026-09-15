#pragma once

#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/editor/gui/actions/HistoryActions.hpp>
#include <lux/engine/ui/UISession.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/editor/gui/GuiView.hpp>

namespace lux::editor::gui
{
    // Shares registration/close mechanics; concrete Pane classes own their drawing and interaction.
    template <class Derived> class DocumentPane : public object::Object<Derived, lux::ui::Pane>, public GuiView
    {
      public:
        DocumentPane(scene::SceneEditor &document, std::string name, std::string title)
            : object::Object<Derived, lux::ui::Pane>(document.dispatcherRef(), lux::ui::PaneId{std::move(name)},
                                                     lux::ui::PaneTypeId{"lux.editor.document-pane"}, std::move(title)),
              document_(document), history_(document.dispatcherRef(), document)
        {
        }

        std::string_view id() const noexcept override
        {
            return lux::ui::Pane::id().name();
        }

        lux::ui::Pane &pane() noexcept override
        {
            return *this;
        }

        [[nodiscard]] EditorResult<void> attach(lux::ui::UISession &ui)
        {
            auto registered = ui.registerPane(*this);
            if (!registered)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "ui.register",
                                                          static_cast<std::uint64_t>(registered.error())});
            }
            registration_ = std::move(*registered);
            auto &router = ui.commandRouter();
            const auto bind = [&](std::string_view name, auto action, auto enabled) -> EditorResult<void>
            {
                const auto command = router.findCommand(lux::ui::UiCommandIdView{name});
                if (!command)
                {
                    return lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "ui.command"});
                }
                auto binding = router.template bind<decltype(action)::value, decltype(enabled)::value>(
                    *command, lux::ui::UiContextId{id()}, *this, history_);
                if (!binding)
                {
                    return lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "ui.bind",
                                                              static_cast<std::uint64_t>(binding.error())});
                }
                commands_.push_back(std::move(*binding));
                return {};
            };
            auto undo =
                bind("lux.edit.undo", std::integral_constant<decltype(&HistoryActions::undo), &HistoryActions::undo>{},
                     std::integral_constant<decltype(&HistoryActions::canUndo), &HistoryActions::canUndo>{});
            if (!undo)
            {
                return undo;
            }
            return bind("lux.edit.redo",
                        std::integral_constant<decltype(&HistoryActions::redo), &HistoryActions::redo>{},
                        std::integral_constant<decltype(&HistoryActions::canRedo), &HistoryActions::canRedo>{});
        }

        void requestClose() noexcept override
        {
            closing_ = true;
            commands_.clear();
            registration_.reset();
        }

        void poll(PollBudget &) override {}

        CloseStatus closeStatus() const override
        {
            return {closing_ ? ECloseState::CLOSED : ECloseState::OPEN, {}};
        }

      protected:
        scene::SceneEditor &document_;
        bool closing_{};

      private:
        HistoryActions history_;
        lux::ui::PaneRegistration registration_;
        std::vector<lux::ui::CommandRegistration> commands_;
    };
} // namespace lux::editor::gui
