#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <algorithm>

namespace lux::editor::application
{
    EditorResult<void> EditorApplication::Impl::reload(commands::SessionTarget target)
    {
        if (reloads_.size() >= 64)
            return cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "reload.results"});
        auto current = sessions_.describe(target.id);
        if (!current)
            return applicationFailure("reload.session", current.error());
        if (!target.based_on || current->current != *target.based_on)
            return applicationFailure("reload.source", sessions::ESessionError::STALE_CONTENT);
        if (!current->binding)
            return applicationFailure("reload.unbound", persistence::EPersistenceError::UNBOUND);
        const bool active = std::ranges::any_of(reloads_, [&](const auto& other) {
            return other.source == *target.based_on || (other.source.session == target.id && !other.result);
        });
        if (active)
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "reload.active"});
        const auto* asset = project_->asset(current->binding->asset);
        if (!asset)
            return cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "reload.asset"});
        auto destination = files_.resolve(asset->source_path);
        if (!destination)
            return applicationFailure("reload.target", destination.error());
        auto bytes = project_->captureSource(asset->id, 64 * 1024 * 1024, destination->expected_version);
        if (!bytes)
            return cxx::unexpected(bytes.error());
        // A command already holds the participating registry scopes. Keep only its immutable factory
        // owner across the read; never retain a Batch while waiting for IO or a user decision.
        auto factory = contributions_.snapshot().sessions().find(current->kind);
        if (!factory)
            return applicationFailure("reload.factory", factory.error());
        sessions::SessionLoadInput input{
            std::move(*bytes),
            asset->id,
            current->binding,
            std::move(*destination),
            64 * 1024 * 1024,
            *target.based_on
        };
        auto operation = sessions::ReloadSessionOperation::start(
            engine_->execution(),
            sessions_,
            writes_,
            std::move(*factory),
            std::move(input)
        );
        if (!operation)
            return applicationFailure("reload.admission", operation.error());
        reloads_.push_back({*target.based_on, std::move(*operation)});
        return {};
    }
    EditorResult<void> EditorApplication::Impl::askReload(commands::SessionTarget target)
    {
        if (phase_ != EApplicationPhase::RUNNING || reload_question_ || save_question_ || last_view_)
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "reload.question"});
        auto current = sessions_.describe(target.id);
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
        views::DetachedView candidate{contracts::CodeLease::builtin(), std::move(*question)};
        auto shown = adopt(candidate, "reload-review");
        if (!shown)
            return cxx::unexpected(shown.error());
        reload_question_ = ReloadQuestion{target, *shown};
        return {};
    }
    EditorResult<void> EditorApplication::Impl::receiveReloadAnswer()
    {
        if (!reload_question_)
            return {};
        std::optional<desktop::ReviewAnswer> answer;
        auto read = [&](lux::ui::Pane& pane) { answer = static_cast<desktop::ReviewView&>(pane).response(); };
        auto borrowed = desktop_->views().withView(reload_question_->view, read);
        if (!borrowed)
            return applicationFailure("reload.question.read", borrowed.error());
        if (!answer)
            return {};
        auto close = desktop_->views().prepareClose(std::span{&reload_question_->view, 1});
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
                auto displayed = desktop_->views().withView(reload_question_->view, reject);
                if (!displayed)
                    return applicationFailure("reload.question.error", displayed.error());
                return {};
            }
        }
        auto closed = desktop_->views().commit(*close);
        if (!closed)
            return applicationFailure("reload.question.commit", closed.error());
        reload_question_.reset();
        return {};
    }
}
