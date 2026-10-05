#include <algorithm>
#include <lux/engine/editor/application/EditorApplicationImpl.hpp>

namespace lux::editor::application
{
    EditorResult<void> EditorApplication::closeView(lux::ui::PaneHandle id)
    {
        if (auto ready = impl_->admission(); !ready)
        {
            return ready;
        }
        Impl::Dispatch scope{impl_->dispatching_};
        return impl_->closeView(id);
    }
    EditorResult<void> EditorApplication::Impl::requestClose(sessions::ContentStamp expected)
    {
        if (phase_ != EApplicationPhase::RUNNING || last_view_ || save_question_ || reload_question_)
        {
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "close.review"});
        }
        if (!content_saving_->hasCapacity(1))
        {
            return cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "close.save-results"});
        }
        auto current = sessions_.describe(expected.session);
        if (!current)
        {
            return applicationFailure("close.content", current.error());
        }
        if (current->current != expected)
        {
            return applicationFailure("close.content", sessions::ESessionError::STALE_CONTENT);
        }
        close_decisions_ = {
            {expected, current->dirty ? sessions::ECloseChoice::CANCEL : sessions::ECloseChoice::DISCARD}
        };
        close_run_decisions_.clear();
        close_destinations_.clear();
        for (const auto& run : run_presentations_)
        {
            if (run.source.session == expected.session && run.run && !run.stopping)
            {
                close_run_decisions_.push_back({*run.run, {}});
            }
        }
        close_application_ = false;
        phase_ = EApplicationPhase::REVIEWING;
        return {};
    }
    EditorResult<void> EditorApplication::Impl::closeView(lux::ui::PaneHandle id)
    {
        if (phase_ != EApplicationPhase::RUNNING || last_view_)
        {
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "close.view.review"});
        }
        auto information = editor_context_.ui().content(desktop_->root(), id);
        if (!information)
        {
            return applicationFailure("close.view", information.error());
        }
        if (information->primary)
        {
            auto all_views = editor_context_.ui().describe(desktop_->root());
            if (!all_views)
            {
                return applicationFailure("close.view.references", all_views.error());
            }
            const auto primary = *information->primary;
            const auto count = std::ranges::count_if(
                *all_views,
                [&](const auto& view)
                { return std::ranges::find(view.content.sessions, primary) != view.content.sessions.end(); }
            );
            if (count == 1)
            {
                auto author = sessions_.describe(primary);
                if (!author)
                {
                    return applicationFailure("close.view.content", author.error());
                }
                auto prompt = desktop::ReviewView::create(
                    messages_.dispatcherRef(),
                    lux::ui::PaneId{"last-view"},
                    {next_review_++,
                     "Last view of this content",
                     "Keep the content open without a view, or close the content?",
                     {desktop::EReviewChoice::KEEP_CONTENT,
                      desktop::EReviewChoice::CLOSE_CONTENT,
                      desktop::EReviewChoice::CANCEL}}
                );
                if (!prompt)
                {
                    return applicationFailure("close.view.question", prompt.error());
                }
                auto* pane = prompt->get();
                auto mounted = desktop_->root().addSubPane(std::move(*prompt));
                if (!mounted)
                {
                    return applicationFailure("close.question.mount", mounted.error());
                }
                auto shown = desktop_->root().identify(*pane);
                if (!shown)
                {
                    return applicationFailure("close.question.identity", shown.error());
                }
                last_view_ = LastViewQuestion{id, author->current, *shown};
                return {};
            }
        }
        auto prepared = editor_context_.ui().prepareClose(desktop_->root(), std::span{&id, 1});
        if (!prepared)
        {
            return applicationFailure("close.view.prepare", prepared.error());
        }
        auto committed = desktop_->root().commit(*prepared);
        if (!committed)
        {
            return applicationFailure("close.view.commit", committed.error());
        }
        for (auto& run : run_presentations_)
        {
            std::erase(run.views, id);
        }
        return {};
    }
    EditorResult<void> EditorApplication::Impl::receiveViewClose()
    {
        if (last_view_)
        {
            std::optional<desktop::ReviewAnswer> answer;
            auto read_answer = [&](lux::ui::Pane& pane)
            {
                if (pane.type() == lux::ui::PaneTypeId{"lux.editor.review"})
                {
                    answer = static_cast<desktop::ReviewView&>(pane).response();
                }
            };
            auto borrowed = desktop_->root().withPane(last_view_->question, read_answer);
            if (!borrowed)
            {
                return applicationFailure("close.view.answer", borrowed.error());
            }
            if (!answer)
            {
                return {};
            }
            const auto decision = *last_view_;
            const bool keep = answer->choice == desktop::EReviewChoice::KEEP_CONTENT;
            // Domain preparation uses the registered output; the review is an ordinary Root child.
            // Their structure is committed together, with no callback between preparation and commit.
            if (keep)
            {
                auto prepared = editor_context_.ui().prepareClose(desktop_->root(), std::span{&decision.view, 1});
                if (!prepared)
                {
                    return applicationFailure("close.view.prepare", prepared.error());
                }
            }
            auto question = desktop_->root().findPane(decision.question);
            if (!question)
            {
                return applicationFailure("close.question.target", question.error());
            }
            std::vector<lux::ui::Pane*> panes{*question};
            if (keep)
            {
                auto view = desktop_->root().findPane(decision.view);
                if (!view)
                {
                    return applicationFailure("close.view.target", view.error());
                }
                panes.push_back(*view);
            }
            auto prepared = desktop_->root().prepareDetach(panes);
            if (!prepared)
            {
                return applicationFailure("close.view.prepare", prepared.error());
            }
            auto committed = desktop_->root().commit(*prepared);
            if (!committed)
            {
                return applicationFailure("close.view.commit", committed.error());
            }
            last_view_.reset();
            if (keep)
            {
                for (auto& run : run_presentations_)
                {
                    std::erase(run.views, decision.view);
                }
            }
            else
            {
                auto view = desktop_->root().findPane(decision.view);
                if (!view)
                {
                    return applicationFailure("close.view.intent", view.error());
                }
                (*view)->dismissCloseRequest();
                if (answer->choice == desktop::EReviewChoice::CLOSE_CONTENT)
                {
                    return requestClose(decision.content);
                }
            }
            return {};
        }
        auto windows = editor_context_.ui().describe(desktop_->root());
        if (!windows)
        {
            return applicationFailure("close.view.requests", windows.error());
        }
        for (const auto& info : *windows)
        {
            auto pane = desktop_->root().findPane(info.handle);
            if (!pane)
            {
                return applicationFailure("close.view.target", pane.error());
            }
            if (!(*pane)->hasCloseRequest())
            {
                continue;
            }
            auto closed = closeView(info.handle);
            if (!closed)
            {
                if (closed.error().code != EEditorError::BUSY)
                {
                    // A refusal ends this intent. Retain its diagnostic; only a new close request
                    // retries it. Preparation may invoke extension code, so revalidate the identity.
                    if (auto current = desktop_->root().findPane(info.handle); current)
                    {
                        (*current)->dismissCloseRequest();
                    }
                }
                return closed;
            }
            if (last_view_)
            {
                break;
            }
        }
        return {};
    }
    desktop::ToolOpening EditorApplication::Impl::toolOpening()
    {
        return [this](views::ViewTypeId type) -> commands::CommandResult<lux::ui::PaneHandle>
        {
            // Original command dispatch pins the immutable catalog through the construction boundary.
            return desktop::showTool(
                desktop_->root(),
                editor_context_.ui(),
                editor_context_.scope(),
                contributions_.snapshot().ui(),
                std::move(type)
            );
        };
    }
} // namespace lux::editor::application
