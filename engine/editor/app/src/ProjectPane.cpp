#include <algorithm>
#include <cstdio>
#include <imgui.h>
#include <lux/engine/editor/detail/EditorImpl.hpp>

namespace lux::editor
{
Editor::ProjectPane::ProjectPane(Editor &editor)
    : Object(editor.messages_.dispatcherRef(), lux::ui::PaneId{"project"}, lux::ui::PaneTypeId{"lux.editor.project"},
             "Project"),
      editor_(editor)
{
}
void Editor::ProjectPane::draw(lux::ui::Frame &frame, lux::ui::PaneDrawContext &)
{
    frame.textWrapped(status_);
    ImGui::SameLine();
    if (frame.smallButton("Restore panes"))
    {
        restorePanes();
    }
    projectActions(frame);
}
void Editor::ProjectPane::report(const EditorFailure &failure)
{
    status_ = failure.domain + ":" + std::to_string(failure.reason) + " " + failure.message;
    std::fprintf(stderr, "%s\n", status_.c_str());
}
void Editor::ProjectPane::nativeClose()
{
    if (!editor_.project_)
    {
        static_cast<void>(editor_.cancelStartup());
        return;
    }
    if (close_choice_ != ECloseChoice::NONE || editor_.closing())
    {
        return;
    }
    const auto review = editor_.beginExitReview();
    if (!review)
    {
        report(review.error());
        return;
    }
    exit_review_ = *review;
    close_decisions_.clear();
    exit_after_close_ = true;
    close_choice_ = ECloseChoice::REVIEW;
    close_interactions_pending_ = true;
    refreshClosingDocuments();
    setVisible(true);
}
void Editor::ProjectPane::poll()
{
    if (!editor_.project_)
    {
        return;
    }
    std::erase_if(documents_, [&](DocumentHandle handle) { return !editor_.document(handle); });
    if (!default_requested_ && !editor_.closing())
    {
        default_requested_ = true;
        const auto &manifest = editor_.project_->manifest();
        const auto found = std::ranges::find(manifest.assets, manifest.default_scene, &ProjectAssetEntry::source_path);
        if (found != manifest.assets.end())
        {
            open(*found);
        }
    }
    for (auto request = requests_.begin(); request != requests_.end();)
    {
        const auto result = editor_.openStatus(*request);
        if (!result || !std::holds_alternative<OpenPending>(*result))
        {
            if (result)
            {
                if (const auto *handle = std::get_if<DocumentHandle>(&*result))
                {
                    attach(*handle);
                }
                else if (const auto *error = std::get_if<EditorFailure>(&*result))
                {
                    report(*error);
                }
                static_cast<void>(editor_.acknowledgeOpen(*request));
            }
            request = requests_.erase(request);
        }
        else
        {
            ++request;
        }
    }
    pollSaves();
    advanceDocumentClose();
}
void Editor::ProjectPane::open(const ProjectAssetEntry &asset)
{
    const auto provider =
        std::ranges::find_if(editor_.config_.providers, [&asset](const auto &value) { return value.accepts(asset); });
    if (provider == editor_.config_.providers.end())
    {
        status_ = "This document type is not available in this editor build";
        return;
    }
    const auto opened =
        editor_.requestOpen({{editor_.project_->manifest().id, asset.id, provider->type}, "project-browser"});
    if (!opened)
    {
        report(opened.error());
        return;
    }
    requests_.push_back(*opened);
    status_ = "Opening " + asset.source_path;
}

void Editor::ProjectPane::projectActions(lux::ui::Frame &frame)
{
    if (!editor_.project_)
    {
        ImGui::SameLine();
        if (frame.smallButton("Cancel startup"))
        {
            static_cast<void>(editor_.cancelStartup());
        }
        return;
    }
    ImGui::BeginDisabled(editor_.closing());
    for (const auto &asset : editor_.project_->manifest().assets)
    {
        ImGui::SameLine();
        if (std::ranges::any_of(editor_.config_.providers,
                                [&asset](const auto &value) { return value.accepts(asset); }) &&
            frame.button(asset.source_path))
        {
            open(asset);
        }
    }
    ImGui::EndDisabled();
    for (const auto handle : documents_)
    {
        auto document = editor_.document(handle);
        if (!document)
        {
            continue;
        }
        const auto summary = document->get().summary();
        const auto history = document->get().reviewClose();
        const auto key = std::to_string(handle.index) + "-" + std::to_string(handle.gen);
        ImGui::PushID(key.c_str());
        ImGui::TextUnformatted(summary.title.c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(summary.read_only || !document->get().saveRequests().empty());
        if (frame.smallButton("Save"))
        {
            beginSave(handle);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(exit_review_.serial != 0);
        if (frame.smallButton("Close"))
        {
            closing_documents_ = {handle};
            close_decisions_.clear();
            exit_after_close_ = false;
            close_choice_ = ECloseChoice::REVIEW;
            close_interactions_pending_ = true;
        }
        ImGui::EndDisabled();
        if (history && !history->clean)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("Unsaved changes");
        }
        for (const auto save : document->get().saveRequests())
        {
            const auto status = document->get().saveStatus(save);
            if (status)
            {
                if (const auto *failed = std::get_if<SaveRetryable>(&*status))
                {
                    ImGui::TextWrapped("Save failed: %s (%llu) %s", failed->failure.domain.c_str(),
                                       static_cast<unsigned long long>(failed->failure.reason),
                                       failed->failure.message.c_str());
                    ImGui::BeginDisabled(!failed->retry_allowed);
                    if (frame.smallButton("Retry save"))
                    {
                        const auto retry = document->get().retrySave(save);
                        if (!retry)
                        {
                            report(retry.error());
                        }
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    if (frame.smallButton("Abandon save"))
                    {
                        const auto abandon = document->get().abandonSave(save);
                        if (!abandon)
                        {
                            report(abandon.error());
                        }
                    }
                }
                else if (const auto *done = std::get_if<SaveSucceeded>(&*status))
                {
                    ImGui::TextDisabled(done->cleanup ? "Saved; awaiting request acknowledgment"
                                                      : "Saved; publication cleanup requires attention");
                }
                else if (std::holds_alternative<SaveAbandoned>(*status))
                {
                    ImGui::TextDisabled("Save abandoned; awaiting request acknowledgment");
                }
                else
                {
                    ImGui::TextDisabled("Saving captured content...");
                }
            }
        }
        ImGui::PopID();
    }
    drawCloseChoice();
}

void Editor::ProjectPane::restorePanes()
{
    editor_.impl_->visitViews(editor_, [](gui::GuiView &view) { view.pane().setVisible(true); });
}

std::vector<SaveRequestId>::iterator Editor::ProjectPane::saveFor(DocumentHandle handle)
{
    return std::ranges::find(saves_, handle, &SaveRequestId::document);
}

void Editor::ProjectPane::beginSave(DocumentHandle handle)
{
    if (saveFor(handle) != saves_.end())
    {
        return;
    }
    auto document = editor_.document(handle);
    if (!document)
    {
        report(document.error());
        return;
    }
    const auto accepted = document->get().requestSave("desktop");
    if (!accepted)
    {
        report(accepted.error());
        close_choice_ = ECloseChoice::REVIEW;
        return;
    }
    saves_.push_back(*accepted);
}

void Editor::ProjectPane::pollSaves()
{
    std::erase_if(saves_, [&](SaveRequestId id) {
        auto document = editor_.document(id.document);
        if (!document)
        {
            return true;
        }
        auto status = document->get().saveStatus(id);
        if (!status)
        {
            report(status.error());
            return true;
        }
        if (const auto *done = std::get_if<SaveSucceeded>(&*status))
        {
            if (!done->cleanup)
            {
                report(done->cleanup.error());
            }
            else
            {
                status_ = "Saved " + document->get().summary().title;
            }
        }
        else if (!std::holds_alternative<SaveAbandoned>(*status))
        {
            return false;
        }
        const auto acknowledged = document->get().acknowledgeSave(id);
        if (!acknowledged)
        {
            report(acknowledged.error());
            return false;
        }
        return true;
    });
}

void Editor::ProjectPane::refreshClosingDocuments()
{
    if (!exit_after_close_)
    {
        return;
    }
    closing_documents_.clear();
    for (const auto &summary : editor_.documents())
    {
        closing_documents_.push_back(summary.handle);
    }
}

bool Editor::ProjectPane::finishClosingInteractions()
{
    for (const auto handle : closing_documents_)
    {
        if (auto document = editor_.document(handle))
        {
            for (const auto &view : document->get().views())
            {
                if (auto *gui = dynamic_cast<gui::GuiView *>(view.get()))
                {
                    const auto finished = gui->finishInteraction();
                    if (!finished)
                    {
                        report(finished.error());
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

void Editor::ProjectPane::advanceDocumentClose()
{
    if (close_choice_ == ECloseChoice::NONE)
    {
        return;
    }
    refreshClosingDocuments();
    if (close_interactions_pending_ && !finishClosingInteractions())
    {
        return;
    }
    close_interactions_pending_ = false;

    std::vector<DocumentCloseDecision> decisions;
    decisions.reserve(closing_documents_.size());
    for (const auto handle : closing_documents_)
    {
        auto document = editor_.document(handle);
        if (!document)
        {
            continue;
        }
        const auto selected = std::ranges::find(close_decisions_, handle, &DocumentCloseDecision::document);
        if (close_choice_ == ECloseChoice::DISCARDING && selected != close_decisions_.end())
        {
            for (const auto request : document->get().saveRequests())
            {
                const auto status = document->get().saveStatus(request);
                if (status &&
                    (std::holds_alternative<SavePending>(*status) || std::holds_alternative<SaveRetryable>(*status)))
                {
                    const auto abandoned = document->get().abandonSave(request);
                    if (!abandoned)
                    {
                        report(abandoned.error());
                    }
                }
            }
        }
        const auto snapshot = document->get().reviewClose();
        if (!snapshot)
        {
            report(snapshot.error());
            return;
        }
        auto choice = EDocumentCloseDecision::CLOSE_CLEAN;
        if (!snapshot->clean)
        {
            if (close_choice_ != ECloseChoice::DISCARDING || selected == close_decisions_.end() ||
                selected->state != snapshot->current || selected->revision != snapshot->revision)
            {
                close_choice_ = ECloseChoice::REVIEW;
                return;
            }
            choice = EDocumentCloseDecision::DISCARD_THIS_STATE;
        }
        decisions.push_back({handle, snapshot->current, snapshot->revision, choice});
    }

    if (exit_after_close_)
    {
        const auto committed = editor_.commitExitReview(exit_review_, decisions);
        if (!committed)
        {
            report(committed.error());
            if (committed.error().code != EEditorError::BUSY)
            {
                close_choice_ = ECloseChoice::REVIEW;
            }
            return;
        }
    }
    else
    {
        // The normal owner poll has rechecked this single-document choice.
        for (const auto &decision : decisions)
        {
            if (auto document = editor_.document(decision.document))
            {
                document->get().requestClose();
            }
        }
    }
    close_choice_ = ECloseChoice::NONE;
    closing_documents_.clear();
    close_decisions_.clear();
}

void Editor::ProjectPane::drawCloseChoice()
{
    constexpr auto popup = "Close documents";
    if (close_choice_ == ECloseChoice::NONE)
    {
        if (ImGui::BeginPopupModal(popup, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        return;
    }
    if (!ImGui::IsPopupOpen(popup))
    {
        ImGui::OpenPopup(popup);
    }
    const auto *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, {0.5F, 0.5F});
    if (!ImGui::BeginPopupModal(popup, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }
    struct PopupScope final
    {
        ~PopupScope()
        {
            ImGui::EndPopup();
        }
    } popup_scope;
    ImGui::TextWrapped("Save changes before closing?");
    if (close_choice_ == ECloseChoice::REVIEW)
    {
        if (ImGui::Button("Save changes"))
        {
            std::fprintf(stderr, "[editor.exit] event=choice action=save scope=%s review=%llu\n",
                         exit_after_close_ ? "editor" : "document",
                         static_cast<unsigned long long>(exit_review_.serial));
            refreshClosingDocuments();
            if (!finishClosingInteractions())
            {
                return;
            }
            close_decisions_.clear();
            close_choice_ = ECloseChoice::SAVING;
            for (const auto handle : closing_documents_)
            {
                if (auto document = editor_.document(handle))
                {
                    const auto history = document->get().reviewClose();
                    if (history && !history->clean)
                    {
                        beginSave(handle);
                    }
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard changes"))
        {
            std::fprintf(stderr, "[editor.exit] event=choice action=discard scope=%s review=%llu\n",
                         exit_after_close_ ? "editor" : "document",
                         static_cast<unsigned long long>(exit_review_.serial));
            refreshClosingDocuments();
            if (!finishClosingInteractions())
            {
                return;
            }
            close_decisions_.clear();
            for (const auto handle : closing_documents_)
            {
                if (auto document = editor_.document(handle))
                {
                    const auto history = document->get().reviewClose();
                    if (!history)
                    {
                        return;
                    }
                    close_decisions_.push_back(
                        {handle, history->current, history->revision, EDocumentCloseDecision::DISCARD_THIS_STATE});
                }
            }
            close_choice_ = ECloseChoice::DISCARDING;
        }
        ImGui::SameLine();
    }
    else
    {
        ImGui::TextDisabled("Waiting for the current save or recovery to finish.");
    }
    if (ImGui::Button("Cancel close"))
    {
        std::fprintf(stderr, "[editor.exit] event=choice action=cancel scope=%s review=%llu\n",
                     exit_after_close_ ? "editor" : "document", static_cast<unsigned long long>(exit_review_.serial));
        if (exit_after_close_)
        {
            const auto cancelled = editor_.cancelExitReview(exit_review_);
            if (!cancelled)
            {
                report(cancelled.error());
                return;
            }
        }
        close_choice_ = ECloseChoice::NONE;
        closing_documents_.clear();
        close_decisions_.clear();
        exit_review_ = {};
        editor_.cancelNativeClose();
        ImGui::CloseCurrentPopup();
    }
}

void Editor::ProjectPane::attach(DocumentHandle handle)
{
    const auto document = editor_.document(handle);
    if (!document)
    {
        report(document.error());
        return;
    }
    const auto found = std::ranges::find(editor_.config_.providers, document->get().summary().key.type,
                                         &gui::GuiDocumentProvider::type);
    if (found == editor_.config_.providers.end())
    {
        return;
    }
    const auto attached =
        found->attach(document->get(), *editor_.impl_->ui, *editor_.impl_->renderer, editor_.impl_->process);
    if (!attached)
    {
        report(attached.error());
        return;
    }
    if (std::ranges::find(documents_, handle) == documents_.end())
    {
        documents_.push_back(handle);
    }
    restorePanes();
    status_ = "Project ready";
}
} // namespace lux::editor
