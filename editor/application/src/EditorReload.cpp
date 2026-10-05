#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <algorithm>

namespace lux::editor::application
{
    EditorResult<void> EditorApplication::Impl::reload(commands::SessionTarget target)
    {
        if (!target.based_on || target.based_on->session != target.id)
        {
            return applicationFailure("reload.source", sessions::ESessionError::STALE_CONTENT);
        }
        if (!reloading_)
        {
            auto service = editor_context_.services().get<ProjectContentReloading>(editor_context_.scope());
            if (!service) return applicationFailure("reload.service", service.error());
            reloading_ = std::move(*service);
        }
        return reloading_->request(*target.based_on);
    }
    EditorResult<void> EditorApplication::Impl::askReload(commands::SessionTarget target)
    {
        if (phase_ != EApplicationPhase::RUNNING || reload_question_ || save_question_ || last_view_)
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "reload.question"});
        auto current = sessions_->describe(target.id);
        if (!current)
            return applicationFailure("reload.question.source", current.error());
        if (!target.based_on || current->current != *target.based_on)
            return applicationFailure("reload.question.source", sessions::ESessionError::STALE_CONTENT);
        if (!current->binding)
            return applicationFailure("reload.question.unbound", persistence::EPersistenceError::UNBOUND);
        if (!current->dirty)
            return reload(target);
        auto question = desktop::ReviewView::create(
            messages_.dispatcherRef(),
            lux::ui::PaneId{"reload-review"},
            {next_review_++,
             "Reload content",
             "Discard these unsaved changes and read the bound source? Cancel to keep editing or save first.",
             {desktop::EReviewChoice::DISCARD, desktop::EReviewChoice::CANCEL}}
        );
        if (!question)
            return applicationFailure("reload.question.create", question.error());
        auto& root = desktop_->root();
        auto* pane = question->get();
        auto mounted = root.addSubPane(std::move(*question));
        if (!mounted)
            return applicationFailure("reload-review.mount", mounted.error());
        auto shown = root.identify(*pane);
        if (!shown)
            return applicationFailure("reload-review.identity", shown.error());
        reload_question_ = ReloadQuestion{target, *shown};
        return {};
    }
    EditorResult<void> EditorApplication::Impl::receiveReloadAnswer()
    {
        if (!reload_question_)
            return {};
        std::optional<desktop::ReviewAnswer> answer;
        auto read = [&](lux::ui::Pane& pane) { answer = static_cast<desktop::ReviewView&>(pane).response(); };
        auto borrowed = desktop_->root().withPane(reload_question_->view, read);
        if (!borrowed)
            return applicationFailure("reload.question.read", borrowed.error());
        if (!answer)
            return {};
        auto pane = desktop_->root().findPane(reload_question_->view);
        if (!pane)
            return applicationFailure("reload-review.target", pane.error());
        auto close = desktop_->root().prepareDetach(**pane);
        if (!close)
            return applicationFailure("reload.question.close", close.error());
        if (answer->choice == desktop::EReviewChoice::DISCARD)
        {
            auto accepted = reload(reload_question_->target);
            if (!accepted)
            {
                const auto* factory = std::any_cast<sessions::SessionFactoryFailure>(&accepted.error().cause);
                const auto* access = std::any_cast<sessions::ESessionError>(&accepted.error().cause);
                const bool busy = accepted.error().code == EEditorError::BUSY ||
                                  (factory && factory->code == sessions::ESessionFactoryError::BUSY) ||
                                  (access && *access == sessions::ESessionError::BUSY);
                if (busy)
                    return {};
                auto reject = [&](lux::ui::Pane& pane) {
                    static_cast<desktop::ReviewView&>(pane).rejectAnswer(
                        accepted.error().domain + ": " + accepted.error().message +
                        "\nNo replacement occurred. Cancel and review the current content before retrying."
                    );
                };
                auto displayed = desktop_->root().withPane(reload_question_->view, reject);
                if (!displayed)
                    return applicationFailure("reload.question.error", displayed.error());
                return {};
            }
        }
        auto closed = desktop_->root().commit(*close);
        if (!closed)
            return applicationFailure("reload.question.commit", closed.error());
        reload_question_.reset();
        return {};
    }
}
