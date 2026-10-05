#include <algorithm>
#include <lux/engine/editor/desktop/ReviewView.hpp>
#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <lux/engine/editor/project/ContentReview.hpp>
#include <lux/engine/editor/sessions/SessionOpening.hpp>
#include <lux/engine/editor/storage/ProjectContentReloading.hpp>
#include <lux/engine/editor/storage/ProjectContentSaving.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>
#include <lux/engine/ui/Root.hpp>
#include <thread>

namespace lux::editor::project
{
    namespace
    {
        bool isBusy(sessions::ESessionError error) noexcept
        {
            return error == sessions::ESessionError::BUSY;
        }
        bool isBusy(const sessions::SessionFactoryFailure& error) noexcept
        {
            return error.code == sessions::ESessionFactoryError::BUSY;
        }
        bool isBusy(persistence::EPersistenceError error) noexcept
        {
            return error == persistence::EPersistenceError::BUSY;
        }
        bool isBusy(lux::ui::EAttachmentError error) noexcept
        {
            return error == lux::ui::EAttachmentError::BUSY;
        }
        bool isBusy(desktop::EUiError error) noexcept
        {
            return error == desktop::EUiError::BUSY;
        }
        bool isBusy(const desktop::UiFailure& error) noexcept
        {
            return isBusy(error.code);
        }
        template <class Error> auto failure(std::string domain, const Error& error)
        {
            return cxx::unexpected(EditorFailure{
                isBusy(error) ? EEditorError::BUSY : EEditorError::SOURCE_FAILURE,
                std::move(domain),
                0,
                {},
                error
            });
        }
        struct Dispatch final
        {
            bool& active;
            explicit Dispatch(bool& value) noexcept : active(value)
            {
                active = true;
            }
            ~Dispatch()
            {
                active = false;
            }
            Dispatch(const Dispatch&) = delete;
            Dispatch& operator=(const Dispatch&) = delete;
        };
    } // namespace
    struct ContentReview::Impl final
    {
        struct SaveQuestion final
        {
            commands::SessionTarget target;
            persistence::ESaveMode mode;
            lux::ui::PaneHandle view;
        };
        struct ReloadQuestion final
        {
            commands::SessionTarget target;
            lux::ui::PaneHandle view;
        };
        std::shared_ptr<sessions::SessionStore> sessions_;
        std::shared_ptr<sessions::SessionOpening> opening_;
        std::shared_ptr<ProjectContentSaving> saving_;
        std::shared_ptr<ProjectContentReloading> reloading_;
        lux::ui::Root& root_;
        desktop::UiRegistry& ui_;
        const std::thread::id owner_{std::this_thread::get_id()};
        bool dispatching_{};
        std::optional<SaveQuestion> save_question_;
        std::optional<ReloadQuestion> reload_question_;
        std::uint64_t next_review_{1};
        Impl(
            std::shared_ptr<sessions::SessionStore> sessions,
            std::shared_ptr<sessions::SessionOpening> opening,
            std::shared_ptr<ProjectContentSaving> saving,
            std::shared_ptr<ProjectContentReloading> reloading,
            lux::ui::Root& root,
            desktop::UiRegistry& ui
        )
            : sessions_(std::move(sessions)), opening_(std::move(opening)), saving_(std::move(saving)),
              reloading_(std::move(reloading)), root_(root), ui_(ui)
        {
        }
        EditorResult<void> admission() const noexcept
        {
            if (owner_ != std::this_thread::get_id())
            {
                return cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "content-review.thread"});
            }
            if (dispatching_)
            {
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "content-review.dispatch"});
            }
            return {};
        }
        EditorResult<persistence::SaveId> save(commands::SessionTarget, persistence::ESaveMode, std::string);
        EditorResult<void> cancelContentPreview(sessions::SessionId);
        EditorResult<void> askSave(commands::SessionTarget, persistence::ESaveMode);
        EditorResult<void> receiveSaveAnswer();
        EditorResult<void> reload(commands::SessionTarget);
        EditorResult<void> askReload(commands::SessionTarget);
        EditorResult<void> receiveReloadAnswer();
        EditorResult<void> saveAll()
        {
            auto ids = sessions_->snapshotIds();
            if (!ids)
            {
                return failure("save-all.contents", ids.error());
            }
            if (!saving_->hasCapacity(ids->size()))
            {
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "save-all.results"});
            }
            for (const auto id : *ids)
            {
                if (auto ended = cancelContentPreview(id); !ended)
                {
                    return ended;
                }
            }
            return saving_->saveAll();
        }
    };
    EditorResult<persistence::SaveId> ContentReview::Impl::save(
        commands::SessionTarget target,
        persistence::ESaveMode mode,
        std::string destination
    )
    {
        const bool is_invalid_source = !target.based_on || target.based_on->session != target.id;
        if (is_invalid_source)
        {
            return failure("save.source", sessions::ESessionError::STALE_CONTENT);
        }
        if (auto ended = cancelContentPreview(target.id); !ended)
        {
            return cxx::unexpected(ended.error());
        }
        return saving_->request(*target.based_on, mode, std::move(destination));
    }
    EditorResult<void> ContentReview::Impl::cancelContentPreview(sessions::SessionId id)
    {
        auto views = ui_.describe(root_);
        if (!views)
        {
            return failure("save.views", views.error());
        }
        for (const auto& view : *views)
        {
            if (std::ranges::find(view.content.sessions, id) == view.content.sessions.end())
            {
                continue;
            }
            auto ended = ui_.cancelPreview(root_, view.handle);
            if (!ended)
            {
                return failure("save.preview", ended.error());
            }
        }
        return {};
    }
    EditorResult<void> ContentReview::Impl::askSave(commands::SessionTarget target, persistence::ESaveMode mode)
    {
        const bool has_question = save_question_.has_value() || reload_question_.has_value();
        if (has_question)
        {
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "save.question"});
        }
        auto info = sessions_->describe(target.id);
        if (!info)
        {
            return failure("save.question.source", info.error());
        }
        const bool is_source_mismatch = !target.based_on || *target.based_on != info->current;
        if (is_source_mismatch)
        {
            return failure("save.question.source", sessions::ESessionError::STALE_CONTENT);
        }
        const auto factory = opening_->factory(target.id);
        if (!factory)
        {
            return failure("save.naming", factory.error());
        }
        if (!(*factory)->descriptor().source)
        {
            return cxx::unexpected(EditorFailure{EEditorError::MISSING_PROVIDER, "save.naming"});
        }
        const auto& suffix = (*factory)->descriptor().source->save_extension;
        auto question = desktop::ReviewView::create(
            root_.dispatcherRef(),
            lux::ui::PaneId{"save-destination"},
            {next_review_++,
             mode == persistence::ESaveMode::EXPORT_COPY ? "Export Copy" : "Save As",
             "Choose a project-relative source path. The captured target is checked again before saving.",
             {desktop::EReviewChoice::SAVE, desktop::EReviewChoice::CANCEL},
             "Source path",
             std::string("Content/Untitled").append(suffix)}
        );
        if (!question)
        {
            return failure("save.question.create", question.error());
        }
        auto& root = root_;
        auto* pane = question->get();
        auto mounted = root.addSubPane(std::move(*question));
        if (!mounted)
        {
            return failure("save-destination.mount", mounted.error());
        }
        auto shown = root.identify(*pane);
        if (!shown)
        {
            return failure("save-destination.identity", shown.error());
        }
        save_question_ = SaveQuestion{target, mode, *shown};
        return {};
    }
    EditorResult<void> ContentReview::Impl::receiveSaveAnswer()
    {
        if (!save_question_)
        {
            return {};
        }
        std::optional<desktop::ReviewAnswer> answer;
        auto read = [&](lux::ui::Pane& pane) { answer = static_cast<desktop::ReviewView&>(pane).response(); };
        auto borrowed = root_.withPane(save_question_->view, read);
        if (!borrowed)
        {
            return failure("save.question.read", borrowed.error());
        }
        if (!answer)
        {
            return {};
        }
        auto pane = root_.findPane(save_question_->view);
        if (!pane)
        {
            return failure("save-destination.target", pane.error());
        }
        auto prepared = root_.prepareDetach(**pane);
        if (!prepared)
        {
            return failure("save.question.close", prepared.error());
        }
        if (answer->choice == desktop::EReviewChoice::SAVE)
        {
            auto admitted = save(save_question_->target, save_question_->mode, answer->text);
            if (!admitted)
            {
                const auto* persistence = std::any_cast<persistence::PersistenceFailure>(&admitted.error().cause);
                const auto* session = std::any_cast<sessions::ESessionError>(&admitted.error().cause);
                const bool temporary =
                    admitted.error().code == EEditorError::BUSY ||
                    (persistence && (persistence->code == persistence::EPersistenceError::BUSY ||
                                     persistence->code == persistence::EPersistenceError::WRITER_ACTIVE)) ||
                    (session && *session == sessions::ESessionError::BUSY);
                if (temporary)
                {
                    return {}; // Retain the answered draft and exact source until admission is available.
                }
                auto reject = [&](lux::ui::Pane& pane)
                {
                    static_cast<desktop::ReviewView&>(pane).rejectAnswer(
                        admitted.error().domain + ": " + admitted.error().message +
                        "\nThe original content and target were retained. Correct the path, or Cancel and start again."
                    );
                };
                auto displayed = root_.withPane(save_question_->view, reject);
                if (!displayed)
                {
                    return failure("save.question.error", displayed.error());
                }
                return {};
            }
        }
        auto closed = root_.commit(*prepared);
        if (!closed)
        {
            return failure("save.question.commit", closed.error());
        }
        save_question_.reset();
        return {};
    }
    EditorResult<void> ContentReview::Impl::reload(commands::SessionTarget target)
    {
        const bool is_source_mismatch = !target.based_on || target.based_on->session != target.id;
        if (is_source_mismatch)
        {
            return failure("reload.source", sessions::ESessionError::STALE_CONTENT);
        }
        return reloading_->request(*target.based_on);
    }
    EditorResult<void> ContentReview::Impl::askReload(commands::SessionTarget target)
    {
        const bool has_question = reload_question_.has_value() || save_question_.has_value();
        if (has_question)
        {
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "reload.question"});
        }
        auto current = sessions_->describe(target.id);
        if (!current)
        {
            return failure("reload.question.source", current.error());
        }
        const bool is_source_mismatch = !target.based_on || current->current != *target.based_on;
        if (is_source_mismatch)
        {
            return failure("reload.question.source", sessions::ESessionError::STALE_CONTENT);
        }
        if (!current->binding)
        {
            return failure("reload.question.unbound", persistence::EPersistenceError::UNBOUND);
        }
        if (!current->dirty)
        {
            return reload(target);
        }
        auto question = desktop::ReviewView::create(
            root_.dispatcherRef(),
            lux::ui::PaneId{"reload-review"},
            {next_review_++,
             "Reload content",
             "Discard these unsaved changes and read the bound source? Cancel to keep editing or save first.",
             {desktop::EReviewChoice::DISCARD, desktop::EReviewChoice::CANCEL}}
        );
        if (!question)
        {
            return failure("reload.question.create", question.error());
        }
        auto& root = root_;
        auto* pane = question->get();
        auto mounted = root.addSubPane(std::move(*question));
        if (!mounted)
        {
            return failure("reload-review.mount", mounted.error());
        }
        auto shown = root.identify(*pane);
        if (!shown)
        {
            return failure("reload-review.identity", shown.error());
        }
        reload_question_ = ReloadQuestion{target, *shown};
        return {};
    }
    EditorResult<void> ContentReview::Impl::receiveReloadAnswer()
    {
        if (!reload_question_)
        {
            return {};
        }
        std::optional<desktop::ReviewAnswer> answer;
        auto read = [&](lux::ui::Pane& pane) { answer = static_cast<desktop::ReviewView&>(pane).response(); };
        auto borrowed = root_.withPane(reload_question_->view, read);
        if (!borrowed)
        {
            return failure("reload.question.read", borrowed.error());
        }
        if (!answer)
        {
            return {};
        }
        auto pane = root_.findPane(reload_question_->view);
        if (!pane)
        {
            return failure("reload-review.target", pane.error());
        }
        auto close = root_.prepareDetach(**pane);
        if (!close)
        {
            return failure("reload.question.close", close.error());
        }
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
                {
                    return {};
                }
                auto reject = [&](lux::ui::Pane& pane)
                {
                    static_cast<desktop::ReviewView&>(pane).rejectAnswer(
                        accepted.error().domain + ": " + accepted.error().message +
                        "\nNo replacement occurred. Cancel and review the current content before retrying."
                    );
                };
                auto displayed = root_.withPane(reload_question_->view, reject);
                if (!displayed)
                {
                    return failure("reload.question.error", displayed.error());
                }
                return {};
            }
        }
        auto closed = root_.commit(*close);
        if (!closed)
        {
            return failure("reload.question.commit", closed.error());
        }
        reload_question_.reset();
        return {};
    }
    ContentReview::ContentReview(
        std::shared_ptr<sessions::SessionStore> sessions,
        std::shared_ptr<sessions::SessionOpening> opening,
        std::shared_ptr<ProjectContentSaving> saving,
        std::shared_ptr<ProjectContentReloading> reloading,
        lux::ui::Root& root,
        desktop::UiRegistry& ui
    )
        : impl_(std::make_unique<
                Impl>(std::move(sessions), std::move(opening), std::move(saving), std::move(reloading), root, ui))
    {
    }
    ContentReview::~ContentReview() = default;
    EditorResult<persistence::SaveId> ContentReview::save(
        commands::SessionTarget target,
        persistence::ESaveMode mode,
        std::string destination
    )
    {
        if (auto ready = impl_->admission(); !ready)
        {
            return cxx::unexpected(ready.error());
        }
        Dispatch dispatch{impl_->dispatching_};
        return impl_->save(target, mode, std::move(destination));
    }
    EditorResult<void> ContentReview::saveAll()
    {
        if (auto ready = impl_->admission(); !ready)
        {
            return ready;
        }
        Dispatch dispatch{impl_->dispatching_};
        return impl_->saveAll();
    }
    EditorResult<void> ContentReview::askSave(commands::SessionTarget target, persistence::ESaveMode mode)
    {
        if (auto ready = impl_->admission(); !ready)
        {
            return ready;
        }
        Dispatch dispatch{impl_->dispatching_};
        return impl_->askSave(target, mode);
    }
    EditorResult<void> ContentReview::askReload(commands::SessionTarget target)
    {
        if (auto ready = impl_->admission(); !ready)
        {
            return ready;
        }
        Dispatch dispatch{impl_->dispatching_};
        return impl_->askReload(target);
    }
    EditorResult<void> ContentReview::update()
    {
        if (auto ready = impl_->admission(); !ready)
        {
            return ready;
        }
        Dispatch dispatch{impl_->dispatching_};
        if (impl_->save_question_)
        {
            return impl_->receiveSaveAnswer();
        }
        return impl_->receiveReloadAnswer();
    }
    std::optional<ContentReviewQuestion> ContentReview::question() const noexcept
    {
        if (impl_->save_question_)
        {
            return ContentReviewQuestion{*impl_->save_question_->target.based_on, impl_->save_question_->view};
        }
        if (impl_->reload_question_)
        {
            return ContentReviewQuestion{*impl_->reload_question_->target.based_on, impl_->reload_question_->view};
        }
        return {};
    }
} // namespace lux::editor::project

namespace lux::editor::project
{
    namespace
    {
        constexpr services::ServiceContract contracts[]{
            services::ServiceContract::forType<ContentReview, ContentReview>(
                services::ServiceNameView{"lux.editor.project.content-review"}
            )
        };
        constexpr services::ServiceDependency dependencies[]{
            {services::ServiceNameView{"lux.editor.sessions"},
             1,
             cxx::typeToken<sessions::SessionStore>(),
             services::EDependencyKind::SHARED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.sessions.opening"},
             1,
             cxx::typeToken<sessions::SessionOpening>(),
             services::EDependencyKind::SHARED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.project.content-saving"},
             1,
             cxx::typeToken<ProjectContentSaving>(),
             services::EDependencyKind::SHARED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.project.content-reloading"},
             1,
             cxx::typeToken<ProjectContentReloading>(),
             services::EDependencyKind::SHARED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.ui.root"},
             1,
             cxx::typeToken<lux::ui::Root>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.ui"},
             1,
             cxx::typeToken<desktop::UiRegistry>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT}
        };
        services::ServiceResult<std::unique_ptr<ContentReview>>
        createReview(services::ServiceResolver& resolver, const services::ServiceConfiguration&) noexcept
        {
            auto sessions = resolver.get<sessions::SessionStore>(0);
            if (!sessions)
            {
                return cxx::unexpected(std::move(sessions.error()));
            }
            auto opening = resolver.get<sessions::SessionOpening>(1);
            if (!opening)
            {
                return cxx::unexpected(std::move(opening.error()));
            }
            auto saving = resolver.get<ProjectContentSaving>(2);
            if (!saving)
            {
                return cxx::unexpected(std::move(saving.error()));
            }
            auto reloading = resolver.get<ProjectContentReloading>(3);
            if (!reloading)
            {
                return cxx::unexpected(std::move(reloading.error()));
            }
            auto root = resolver.require<lux::ui::Root>(4);
            if (!root)
            {
                return cxx::unexpected(std::move(root.error()));
            }
            auto ui = resolver.require<desktop::UiRegistry>(5);
            if (!ui)
            {
                return cxx::unexpected(std::move(ui.error()));
            }
            return std::make_unique<ContentReview>(
                std::move(*sessions),
                std::move(*opening),
                std::move(*saving),
                std::move(*reloading),
                root->get(),
                ui->get()
            );
        }
    } // namespace
    constinit const services::ServiceDescriptor kContentReviewService = []
    {
        auto descriptor = services::ServiceDescriptor::forType<ContentReview, createReview>(
            services::ServiceNameView{"lux.editor.project.content-review"},
            contracts,
            dependencies
        );
        descriptor.retention = services::EServiceRetention::SCOPED;
        descriptor.affinity = services::EServiceAffinity::OWNER;
        return descriptor;
    }();
} // namespace lux::editor::project
