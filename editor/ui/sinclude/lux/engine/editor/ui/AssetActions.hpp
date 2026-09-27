#pragma once

#include <lux/engine/editor/AssetEditing.hpp>
#include <lux/engine/editor/AssetSave.hpp>
#include <lux/engine/editor/ui/SaveAllRequest.hpp>
#include <lux/engine/ui/Command.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <array>
#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Pane.hpp>

namespace lux::editor::ui
{
    // Reusable controls, with no asset model, decoder, publication or task ownership.
    template <class Tool> class TAssetActions final : public lux::ui::Element
    {
        enum class EAction : std::uint8_t
        {
            NONE,
            NEW,
            SAVE,
            SAVE_AS,
            RELOAD,
            CLEAR,
            REVIEW_SAVE,
            DISCARD,
            CANCEL,
            RETRY,
            ABANDON
        };

    public:
        TAssetActions(lux::ui::Element& parent, Tool& tool, const ProjectStorage& project, EditorResult<void>& status)
            : lux::ui::Element(parent, lux::ui::ElementId{"asset-actions"}), tool_(tool), project_(project),
              layout_(*this, lux::ui::ElementId{"layout"}),
              buttons_(layout_, lux::ui::ElementId{"buttons"}, lux::ui::ELayoutType::HORIZONTAL),
              new_(buttons_, lux::ui::ElementId{"new"}, "New"), save_(buttons_, lux::ui::ElementId{"save"}, "Save"),
              save_as_(buttons_, lux::ui::ElementId{"save-as"}, "Save As"),
              reload_(buttons_, lux::ui::ElementId{"reload"}, "Reload"),
              clear_(buttons_, lux::ui::ElementId{"clear"}, "Clear"), path_(layout_, lux::ui::ElementId{"path"}),
              message_(layout_, lux::ui::ElementId{"message"}),
              review_(
                  tool,
                  lux::ui::PaneId{std::string(tool.id().name()) + "/asset-review"},
                  lux::ui::PaneTypeId{"lux.editor.asset-review"},
                  "Unsaved changes"
              ),
              review_layout_(review_, lux::ui::ElementId{"review"}),
              prompt_(review_layout_, lux::ui::ElementId{"prompt"}, "Save the current asset before continuing?"),
              review_path_(review_layout_, lux::ui::ElementId{"path"}),
              choices_(review_layout_, lux::ui::ElementId{"choices"}, lux::ui::ELayoutType::HORIZONTAL),
              review_save_(choices_, lux::ui::ElementId{"save"}, "Save"),
              discard_(choices_, lux::ui::ElementId{"discard"}, "Discard"),
              cancel_(choices_, lux::ui::ElementId{"cancel"}, "Cancel"),
              recovery_(layout_, lux::ui::ElementId{"recovery"}, lux::ui::ELayoutType::HORIZONTAL),
              retry_(recovery_, lux::ui::ElementId{"retry"}, "Retry save"),
              abandon_(recovery_, lux::ui::ElementId{"abandon"}, "Abandon save")
        {
            setStretch({1, 0});
            path_.setHint("/Project/path for first save or Save As");
            review_path_.setHint("/Project/path required for a new asset");
            review_.setContent(review_layout_);
            review_.setModal(true);
            review_.setVisible(false);
            new_.setVisible(requires(Tool& tool) { tool.newAsset(); });
            save_as_.setVisible(requires(Tool& tool) { tool.requestSaveAs(std::string_view{}); });
            const std::array buttons{
                &new_,
                &save_,
                &save_as_,
                &reload_,
                &clear_,
                &review_save_,
                &discard_,
                &cancel_,
                &retry_,
                &abandon_
            };
            const std::array actions{
                EAction::NEW,
                EAction::SAVE,
                EAction::SAVE_AS,
                EAction::RELOAD,
                EAction::CLEAR,
                EAction::REVIEW_SAVE,
                EAction::DISCARD,
                EAction::CANCEL,
                EAction::RETRY,
                EAction::ABANDON
            };
            for (std::size_t i{}; i < buttons.size(); ++i)
                connections_[i] = detail::takeConnection(
                    object::LuxObject::connect(
                        buttons[i],
                        &lux::ui::Button::activated,
                        [this, action = actions[i]]() noexcept {
                            action_ = action;
                            target_ = tool_.historyId();
                        }
                    ),
                    status
                );
            connections_.back() = detail::takeConnection(
                object::LuxObject::connect(
                    &review_,
                    &lux::ui::Pane::closeRequested,
                    [this]() noexcept {
                        action_ = EAction::CANCEL;
                        target_ = tool_.historyId();
                    }
                ),
                status
            );
        }

        void command(object::EventView& event) noexcept
        {
            if (auto* request = event.getIf<SaveAllRequest>())
            {
                event.accept();
                const auto view = tool_.historyView();
                request->applicable = view && !view->history.clean;
                if (request->start && request->applicable)
                {
                    const bool busy =
                        tool_.assetStatus().phase != EAssetEditPhase::IDLE || save_dialog_ || save_request_.serial;
                    if (busy)
                    {
                        request->result = lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "save-all.target"});
                        return;
                    }
                    all_save_pending_ = true;
                    all_save_result_ = {};
                    action_ = EAction::SAVE;
                    target_ = tool_.historyId();
                }
                request->pending = all_save_pending_;
                request->result = all_save_result_;
                return;
            }
            auto* request = event.getIf<lux::ui::Command>();
            if (!request)
                return;
            const auto id = request->id.name();
            const auto action = id == "lux.asset.save"      ? EAction::SAVE
                                : id == "lux.asset.save-as" ? EAction::SAVE_AS
                                                            : EAction::NONE;
            if (action == EAction::NONE)
                return;
            event.accept();
            request->enabled = tool_.assetStatus().phase == EAssetEditPhase::IDLE && !tool_.assetId().isNull();
            request->result = lux::ui::ECommandDispatchResult::DISABLED;
            if (request->phase == lux::ui::ECommandPhase::EXECUTE && request->enabled)
            {
                target_ = tool_.historyId();
                action_ = action;
                request->result = lux::ui::ECommandDispatchResult::EXECUTED;
            }
        }

    private:
        lux::ui::SizeHint sizeHintContent() noexcept override
        {
            return layout_.sizeHint();
        }
        lux::ui::SizeHint measureContent(float width) noexcept override
        {
            return layout_.measure(width);
        }
        void arrangeContent() noexcept override
        {
            layout_.arrange({{}, rect().size});
        }
        void draw() noexcept override
        {
            drawChild(layout_);
        }
        void update() noexcept override
        {
            if (action_ != EAction::NONE)
            {
                const auto action = std::exchange(action_, EAction::NONE);
                if (target_ != tool_.historyId())
                {
                    message_.setText("The asset changed before this action could be applied");
                    all_save_pending_ = false;
                    all_save_result_ =
                        lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "save.target-changed"});
                }
                else
                {
                    EditorResult<void> result;
                    if (action == EAction::NEW)
                    {
                        if constexpr (requires { tool_.newAsset(); })
                            result = tool_.newAsset();
                    }
                    else if (action == EAction::RELOAD)
                        result = tool_.reloadAsset();
                    else if (action == EAction::CLEAR)
                        result = tool_.clearAsset();
                    else if (action == EAction::REVIEW_SAVE)
                    {
                        if (save_dialog_)
                        {
                            result = tool_.finishEditing();
                            if (result)
                            {
                                auto saved = tool_.requestSaveAs(review_path_.value());
                                if (saved)
                                {
                                    save_request_ = *saved;
                                    save_dialog_ = false;
                                    prompt_.setText("Save the current asset before continuing?");
                                }
                                else
                                    result = lux::cxx::unexpected(saved.error());
                            }
                        }
                        else
                            result = tool_.reviewAsset(EAssetChangeDecision::SAVE, review_path_.value());
                    }
                    else if (action == EAction::DISCARD)
                        result = tool_.reviewAsset(EAssetChangeDecision::DISCARD);
                    else if (action == EAction::CANCEL)
                    {
                        if (save_dialog_)
                        {
                            save_dialog_ = false;
                            prompt_.setText("Save the current asset before continuing?");
                            all_save_pending_ = false;
                            all_save_result_ =
                                lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "save-all.cancelled"});
                        }
                        else
                            result = tool_.reviewAsset(EAssetChangeDecision::CANCEL);
                    }
                    else if (action == EAction::RETRY || action == EAction::ABANDON)
                        result = action == EAction::RETRY ? tool_.retrySave(recovery_request_)
                                                          : tool_.abandonSave(recovery_request_);
                    else
                    {
                        result = tool_.finishEditing();
                        if (result)
                        {
                            const bool existing = project_.asset(tool_.assetId()) != nullptr;
                            auto saved = [&]() -> EditorResult<SaveRequestId> {
                                if (action == EAction::SAVE && existing)
                                    return tool_.requestSave("toolbar");
                                if constexpr (requires { tool_.requestSaveAs(path_.value()); })
                                {
                                    if (path_.value().empty() || action == EAction::SAVE_AS)
                                    {
                                        save_dialog_ = true;
                                        prompt_.setText("Choose an asset VFS path for saving");
                                        review_path_.setValue(path_.value());
                                        return SaveRequestId{};
                                    }
                                    return tool_.requestSaveAs(path_.value());
                                }
                                return lux::cxx::unexpected(
                                    EditorFailure{EEditorError::INVALID_STATE, "asset.save.target"}
                                );
                            }();
                            if (saved)
                                save_request_ = *saved;
                            else
                                result = lux::cxx::unexpected(saved.error());
                        }
                    }
                    message_.setText(result ? "" : result.error().domain + ": " + result.error().message);
                    if (!result && all_save_pending_)
                    {
                        all_save_pending_ = false;
                        all_save_result_ = lux::cxx::unexpected(result.error());
                    }
                }
            }
            const auto& status = tool_.assetStatus();
            const bool idle = status.phase == EAssetEditPhase::IDLE;
            const bool has_asset = !tool_.assetId().isNull();
            buttons_.setEnabled(idle);
            save_.setEnabled(has_asset);
            save_as_.setEnabled(has_asset);
            reload_.setEnabled(has_asset && project_.asset(tool_.assetId()));
            clear_.setEnabled(has_asset);
            const bool reviewing = status.phase == EAssetEditPhase::REVIEW;
            review_.setVisible(reviewing || save_dialog_);
            discard_.setVisible(!save_dialog_);
            recovery_request_ = {};
            for (const auto request : tool_.saveRequests())
            {
                const auto current = tool_.saveStatus(request);
                if (current && std::holds_alternative<SaveRetryable>(*current))
                {
                    recovery_request_ = request;
                    retry_.setEnabled(std::get<SaveRetryable>(*current).retry_allowed);
                    break;
                }
            }
            recovery_.setVisible(recovery_request_.serial != 0);
            if (status.failure)
                message_.setText(status.failure->domain + ": " + status.failure->message);
            if (save_request_.serial)
            {
                const auto saved = tool_.saveStatus(save_request_);
                if (!saved)
                    save_request_ = {};
                else if (std::holds_alternative<SaveSucceeded>(*saved) || std::holds_alternative<SaveAbandoned>(*saved))
                {
                    if (std::holds_alternative<SaveAbandoned>(*saved))
                        all_save_result_ =
                            lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "save-all.abandoned"});
                    all_save_pending_ = false;
                    if (tool_.acknowledgeSave(save_request_))
                        save_request_ = {};
                }
                else if (const auto* failure = std::get_if<SaveRetryable>(&*saved))
                {
                    message_.setText(failure->failure.domain + ": " + failure->failure.message);
                    all_save_pending_ = false;
                    all_save_result_ = lux::cxx::unexpected(failure->failure);
                }
            }
        }
        Tool& tool_;
        const ProjectStorage& project_;
        EAction action_{};
        editing::HistoryId target_;
        SaveRequestId save_request_;
        bool save_dialog_{}, all_save_pending_{};
        EditorResult<void> all_save_result_;
        lux::ui::Layout layout_, buttons_;
        lux::ui::Button new_, save_, save_as_, reload_, clear_;
        lux::ui::TextEdit path_;
        lux::ui::Label message_;
        lux::ui::Pane review_;
        lux::ui::Layout review_layout_;
        lux::ui::Label prompt_;
        lux::ui::TextEdit review_path_;
        lux::ui::Layout choices_;
        lux::ui::Button review_save_, discard_, cancel_;
        lux::ui::Layout recovery_;
        lux::ui::Button retry_, abandon_;
        SaveRequestId recovery_request_;
        std::array<object::Connection, 11> connections_;
    };
}
