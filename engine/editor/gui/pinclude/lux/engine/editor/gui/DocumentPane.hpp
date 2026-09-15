#pragma once

#include <lux/engine/editor/gui/actions/HistoryActions.hpp>
#include <lux/engine/ui/UISession.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/editor/gui/GuiView.hpp>
#include <lux/engine/ui/Frame.hpp>
#include <imgui.h>

namespace lux::editor::gui
{
    // Shares registration/close mechanics; concrete Pane classes own their drawing and interaction.
    template <class Derived, class Document> class DocumentPane : public object::Object<Derived, lux::ui::Pane>, public GuiView
    {
      public:
        DocumentPane(Document &document, std::string name, std::string title)
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

        void poll(PollBudget &) override
        {
            if (!published_save_.serial)
            {
                return;
            }
            const auto status = document_.saveStatus(published_save_);
            if (!status)
            {
                publication_message_ = status.error().domain + ": " + status.error().message;
                return;
            }
            if (const auto* done = std::get_if<SaveSucceeded>(&*status))
            {
                publication_message_ = done->cleanup ? "Source and compiled asset published" :
                    "Published; cleanup requires attention: " + done->cleanup.error().message;
            }
            else if (std::holds_alternative<SaveAbandoned>(*status))
            {
                publication_message_ = "Publication abandoned";
            }
            else
            {
                return;
            }
            if (document_.acknowledgeSave(published_save_))
            {
                published_save_ = {};
            }
        }

        CloseStatus closeStatus() const override
        {
            return {closing_ ? ECloseState::CLOSED : ECloseState::OPEN, {}};
        }

      protected:
        void trackPublication(SaveRequestId request)
        {
            published_save_ = request;
            publication_message_.clear();
        }
        void drawPublication(lux::ui::Frame& frame)
        {
            if (!publication_message_.empty())
            {
                frame.text(publication_message_);
            }
            if (!published_save_.serial)
            {
                return;
            }
            const auto status = document_.saveStatus(published_save_);
            if (!status)
            {
                frame.text(status.error().message);
                return;
            }
            if (const auto* failed = std::get_if<SaveRetryable>(&*status))
            {
                ImGui::TextWrapped("Publication failed: %s (%llu) %s", failed->failure.domain.c_str(),
                    static_cast<unsigned long long>(failed->failure.reason), failed->failure.message.c_str());
                if (ImGui::SmallButton("Retry publication"))
                {
                    const auto retried = document_.retrySave(published_save_);
                    if (!retried)
                    {
                        publication_message_ = retried.error().domain + ": " + retried.error().message;
                    }
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Abandon publication"))
                {
                    const auto abandoned = document_.abandonSave(published_save_);
                    if (!abandoned)
                    {
                        publication_message_ = abandoned.error().domain + ": " + abandoned.error().message;
                    }
                }
            }
            else
            {
                frame.textMuted("Publishing captured source and compiled asset...");
            }
        }
        Document &document_;
        bool closing_{};

      private:
        SaveRequestId published_save_;
        std::string publication_message_;
        HistoryActions history_;
        lux::ui::PaneRegistration registration_;
        std::vector<lux::ui::CommandRegistration> commands_;
    };
} // namespace lux::editor::gui
