#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <algorithm>

namespace lux::editor::application
{
    EditorResult<void> EditorApplication::closeView(views::ViewId id)
    {
        if (auto ready = impl_->admission(); !ready)
            return ready;
        Impl::Dispatch scope{impl_->dispatching_};
        return impl_->closeView(id);
    }
    EditorResult<void> EditorApplication::Impl::requestClose(sessions::ContentStamp expected)
    {
        if (phase_ != EApplicationPhase::RUNNING || last_view_ || save_question_ || reload_question_)
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "close.review"});
        if (!content_saving_->hasCapacity(1))
            return cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "close.save-results"});
        auto current = sessions_.describe(expected.session);
        if (!current)
            return applicationFailure("close.content", current.error());
        if (current->current != expected)
            return applicationFailure("close.content", sessions::ESessionError::STALE_CONTENT);
        close_decisions_ = {
            {expected, current->dirty ? sessions::ECloseChoice::CANCEL : sessions::ECloseChoice::DISCARD}
        };
        close_run_decisions_.clear();
        close_destinations_.clear();
        for (const auto& run : run_presentations_)
            if (run.source.session == expected.session && run.run && !run.stopping)
                close_run_decisions_.push_back({*run.run, {}});
        close_application_ = false;
        phase_ = EApplicationPhase::REVIEWING;
        return {};
    }
    EditorResult<void> EditorApplication::Impl::closeView(views::ViewId id)
    {
        if (phase_ != EApplicationPhase::RUNNING || last_view_)
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "close.view.review"});
        auto information = desktop_->views().describe(id);
        if (!information)
            return applicationFailure("close.view", information.error());
        if (information->content.primary)
        {
            auto all_views = desktop_->views().describeAll();
            if (!all_views)
                return applicationFailure("close.view.references", all_views.error());
            const auto primary = *information->content.primary;
            const auto count = std::ranges::count_if(
                *all_views,
                [&](const auto& view)
                { return std::ranges::find(view.content.sessions, primary) != view.content.sessions.end(); }
            );
            if (count == 1)
            {
                auto author = sessions_.describe(primary);
                if (!author)
                    return applicationFailure("close.view.content", author.error());
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
                    return applicationFailure("close.view.question", prompt.error());
                views::DetachedView candidate{contracts::CodeLease::builtin(), std::move(*prompt)};
                auto shown = adopt(candidate, "last-view");
                if (!shown)
                    return cxx::unexpected(shown.error());
                last_view_ = LastViewQuestion{id, author->current, *shown};
                return {};
            }
        }
        auto prepared = desktop_->views().prepareClose(std::span{&id, 1});
        if (!prepared)
            return applicationFailure("close.view.prepare", prepared.error());
        auto committed = desktop_->views().commit(*prepared);
        if (!committed)
            return applicationFailure("close.view.commit", committed.error());
        for (auto& run : run_presentations_)
            std::erase(run.views, id);
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
                    answer = static_cast<desktop::ReviewView&>(pane).response();
            };
            auto borrowed = desktop_->views().withView(last_view_->question, read_answer);
            if (!borrowed)
                return applicationFailure("close.view.answer", borrowed.error());
            if (!answer)
                return {};
            const auto decision = *last_view_;
            const std::array ids{decision.question, decision.view};
            const bool keep = answer->choice == desktop::EReviewChoice::KEEP_CONTENT;
            auto prepared = desktop_->views().prepareClose(std::span{ids}.first(keep ? 2 : 1));
            if (!prepared)
                return applicationFailure("close.view.prepare", prepared.error());
            auto committed = desktop_->views().commit(*prepared);
            if (!committed)
                return applicationFailure("close.view.commit", committed.error());
            last_view_.reset();
            if (keep)
            {
                for (auto& run : run_presentations_)
                    std::erase(run.views, decision.view);
            }
            else
            {
                auto dismissed = desktop_->views().dismissCloseIntent(decision.view);
                if (!dismissed)
                    return applicationFailure("close.view.intent", dismissed.error());
                if (answer->choice == desktop::EReviewChoice::CLOSE_CONTENT)
                    return requestClose(decision.content);
            }
            return {};
        }
        auto requests = desktop_->views().closeIntents();
        if (!requests)
            return applicationFailure("close.view.requests", requests.error());
        for (auto id : *requests)
        {
            auto closed = closeView(id);
            if (!closed)
                return closed;
            if (last_view_)
                break;
        }
        return {};
    }
    desktop::ToolOpening EditorApplication::Impl::toolOpening()
    {
        return [host = &desktop_->views(), catalog = &contributions_, dispatcher = messages_.dispatcherRef()](
                   views::ViewTypeId type
               ) -> commands::CommandResult<views::ViewId>
        {
            // Executed inside the original CommandRegistry dispatch, which excludes contribution
            // publication. Pin the current catalog without attempting a recursive read batch.
            return desktop::showTool(*host, catalog->snapshot().views(), dispatcher, std::move(type));
        };
    }
} // namespace lux::editor::application
