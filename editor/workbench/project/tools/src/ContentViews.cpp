#include <algorithm>
#include <lux/engine/editor/extensions/Contributions.hpp>
#include <lux/engine/editor/project/ContentViews.hpp>
#include <lux/engine/editor/project/ProjectView.hpp>
#include <lux/engine/editor/sessions/SessionCommands.hpp>
#include <lux/engine/editor/storage/ProjectContentOpening.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>
#include <lux/engine/log/Log.hpp>
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
        bool isBusy(const extensions::ContributionFailure& error) noexcept
        {
            return error.code == extensions::EContributionError::BUSY;
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
    struct ContentViews::Impl final
    {
        struct OpenPresentation final
        {
            sessions::OpenAssetId operation;
            std::optional<lux::ui::PaneHandle> view;
            std::optional<EditorFailure> failure;
            bool cancelled{};
        };
        std::shared_ptr<sessions::SessionStore> sessions_;
        std::shared_ptr<sessions::SessionOpening> opening_;
        std::shared_ptr<persistence::IArtifactStore> files_;
        ProjectStorage& project_;
        lux::ui::Root& root_;
        desktop::UiRegistry& ui_;
        services::ServiceScope& scope_;
        extensions::ContributionRegistry& contributions_;
        const std::thread::id owner_{std::this_thread::get_id()};
        bool dispatching_{};
        bool suspended_{};
        std::vector<OpenPresentation> opens_;
        std::vector<AssetReference> intents_;
        std::uint64_t next_view_{1};
        sessions::SessionCreation creation_;
        commands::CommandEntry::Query creation_available_;
        ProjectView::Open asset_open_;
        Impl(
            std::shared_ptr<sessions::SessionStore> sessions,
            std::shared_ptr<sessions::SessionOpening> opening,
            std::shared_ptr<persistence::IArtifactStore> files,
            ProjectStorage& project,
            lux::ui::Root& root,
            desktop::UiRegistry& ui,
            services::ServiceScope& scope,
            extensions::ContributionRegistry& contributions
        )
            : sessions_(std::move(sessions)), opening_(std::move(opening)), files_(std::move(files)), project_(project),
              root_(root), ui_(ui), scope_(scope), contributions_(contributions)
        {
            opens_.reserve(64);
            intents_.reserve(64);
        }
        EditorResult<void> admission(bool business = false) const noexcept
        {
            if (owner_ != std::this_thread::get_id())
            {
                return cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "content-views.thread"});
            }
            if (dispatching_)
            {
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "content-views.dispatch"});
            }
            const bool is_closed = business && (suspended_ || !scope_.isOpen());
            if (is_closed)
            {
                return cxx::unexpected(EditorFailure{EEditorError::CLOSING, "content-views.suspended"});
            }
            return {};
        }
        EditorResult<lux::ui::PaneHandle> adopt(std::unique_ptr<lux::ui::Pane, object::ObjectDeleter>&);
        EditorResult<sessions::OpenAssetId> open(AssetReference);
        EditorResult<sessions::OpenAssetId> openCaptured(AssetReference, const extensions::ContributionSnapshot&);
        EditorResult<sessions::OpenAssetId> create(sessions::SessionPreparation);
        EditorResult<lux::ui::PaneHandle> show(sessions::SessionId, bool);
        EditorResult<lux::ui::PaneHandle> makeContentView(
            views::ViewContent,
            bool,
            const extensions::ContributionSnapshot&,
            std::optional<views::ViewRestoreKey> = {},
            std::optional<views::ViewTypeId> = {}
        );
    };
    EditorResult<lux::ui::PaneHandle> ContentViews::Impl::adopt(
        std::unique_ptr<lux::ui::Pane, object::ObjectDeleter>& candidate
    )
    {
        auto* pane = candidate.get();
        auto mounted = root_.addSubPane(std::move(candidate));
        if (!mounted)
        {
            return failure("view.mount", mounted.error());
        }
        auto id = root_.identify(*pane);
        if (!id)
        {
            return failure("view.identity", id.error());
        }
        return *id;
    }
    EditorResult<sessions::OpenAssetId> ContentViews::Impl::open(AssetReference reference)
    {
        std::optional<EditorResult<sessions::OpenAssetId>> result;
        auto prepare = [&](const extensions::ContributionSnapshot& snapshot) -> extensions::ContributionResult<void>
        {
            result.emplace(openCaptured(reference, snapshot));
            return {};
        };
        auto guarded = contributions_.withSnapshot(prepare);
        if (!guarded)
        {
            return failure("open.catalog", guarded.error());
        }
        return std::move(*result);
    }
    EditorResult<sessions::OpenAssetId> ContentViews::Impl::openCaptured(
        AssetReference reference,
        const extensions::ContributionSnapshot& snapshot
    )
    {
        if (suspended_ || !scope_.isOpen())
        {
            return cxx::unexpected(EditorFailure{EEditorError::CLOSING, "content.open"});
        }
        if (opens_.size() == 64)
        {
            return cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "content.open"});
        }
        auto opened = openProjectContent(project_, *files_, *opening_, reference, snapshot.sessions());
        if (!opened)
        {
            return cxx::unexpected(opened.error());
        }
        opens_.push_back({*opened});
        return *opened;
    }

    EditorResult<sessions::OpenAssetId> ContentViews::Impl::create(sessions::SessionPreparation data)
    {
        if (suspended_ || !scope_.isOpen())
        {
            return cxx::unexpected(EditorFailure{EEditorError::CLOSING, "content.create"});
        }
        if (opens_.size() >= 64)
        {
            return cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "content.create"});
        }
        std::optional<sessions::SessionFactoryResult<sessions::OpenAssetId>> prepared;
        auto install = [&](const extensions::ContributionSnapshot& snapshot) -> extensions::ContributionResult<void>
        {
            auto input = std::move(data);
            prepared.emplace(opening_->create(
                project_.catalogModel().reference({}).project_instance,
                std::move(input),
                snapshot.sessions()
            ));
            return {};
        };
        auto guarded = contributions_.withSnapshot(install);
        if (!guarded)
        {
            return failure("content.catalog", guarded.error());
        }
        auto& installed = *prepared;
        if (!installed)
        {
            return failure("content.create", installed.error());
        }
        opens_.push_back({*installed});
        return *installed;
    }
    EditorResult<lux::ui::PaneHandle> ContentViews::Impl::show(sessions::SessionId id, bool another_view)
    {
        std::optional<EditorResult<lux::ui::PaneHandle>> result;
        auto prepare = [&](const extensions::ContributionSnapshot& snapshot) -> extensions::ContributionResult<void>
        {
            result.emplace(makeContentView({{id}, id}, another_view, snapshot));
            return {};
        };
        auto guarded = contributions_.withSnapshot(prepare);
        if (!guarded)
        {
            return failure("show.catalog", guarded.error());
        }
        return std::move(*result);
    }
    EditorResult<lux::ui::PaneHandle> ContentViews::Impl::makeContentView(
        views::ViewContent association,
        bool another_view,
        const extensions::ContributionSnapshot& snapshot,
        std::optional<views::ViewRestoreKey> restore_key,
        std::optional<views::ViewTypeId> preferred
    )
    {
        if (!association.valid())
        {
            return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "show.content"});
        }
        std::optional<views::ViewTypeId> selected = preferred;
        std::string title;
        for (const auto id : association.sessions)
        {
            auto info = sessions_->describe(id);
            if (!info)
            {
                return failure("show.session", info.error());
            }
            auto candidate = snapshot.ui().selectContent(info->kind, selected);
            if (!candidate)
            {
                return failure("show.provider", candidate.error());
            }
            selected = views::ViewTypeId{candidate->descriptor().type.name()};
            if (association.primary == id && info->binding)
            {
                title = info->binding->location;
            }
        }
        if (!selected)
        {
            return cxx::unexpected(EditorFailure{EEditorError::MISSING_PROVIDER, "show.provider"});
        }
        auto views = ui_.describe(root_);
        if (!views)
        {
            return failure("show.views", views.error());
        }
        std::optional<lux::ui::PaneHandle> existing;
        for (const auto& view : *views)
        {
            if (view.type != *selected)
            {
                continue;
            }
            const bool is_restore_target = restore_key && view.restore_key == *restore_key;
            const bool is_reusable = !another_view && !restore_key && view.content == association;
            if (is_reusable)
            {
                auto pane = root_.findPane(view.handle);
                if (!pane)
                {
                    return failure("show.target", pane.error());
                }
                (*pane)->setVisible(true);
                if (!root_.requestFocus(**pane))
                {
                    return cxx::unexpected(EditorFailure{EEditorError::BUSY, "show.focus"});
                }
                return view.handle;
            }
            if (is_restore_target)
            {
                if (!view.content.sessions.empty() && view.content != association)
                {
                    return cxx::unexpected(EditorFailure{
                        EEditorError::STALE_REQUEST,
                        "recovery.binding",
                        0,
                        "The matching window already displays different content; original binding retained."
                    });
                }
                if (view.content == association)
                {
                    return view.handle;
                }
                existing = view.handle;
                break;
            }
        }
        const bool is_full = !existing && views->size() == 64;
        if (is_full || next_view_ == UINT64_MAX)
        {
            return cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "show.views"});
        }
        const auto name = "content-" + std::to_string(next_view_++);
        if (existing)
        {
            auto rebound = ui_.rebind(root_, *existing, association);
            if (!rebound)
            {
                return failure("recovery.binding", rebound.error());
            }
            return *existing;
        }
        auto factory = snapshot.ui().find(selected->view());
        if (!factory)
        {
            return failure("view.factory", factory.error());
        }
        auto view = ui_.create(
            *factory,
            scope_,
            {root_.dispatcherRef(),
             lux::ui::PaneId{name},
             association,
             {factory->descriptor().schema, {}},
             restore_key ? *restore_key : views::ViewRestoreKey{name}}
        );
        if (!view)
        {
            return failure("view.create", view.error());
        }
        if (!title.empty())
        {
            (*view)->setTitle(title);
        }
        auto adopted = adopt(*view);
        if (!adopted)
        {
            return cxx::unexpected(adopted.error());
        }
        return *adopted;
    }

    ContentViews::ContentViews(
        std::shared_ptr<sessions::SessionStore> sessions,
        std::shared_ptr<sessions::SessionOpening> opening,
        std::shared_ptr<persistence::IArtifactStore> files,
        ProjectStorage& project,
        lux::ui::Root& root,
        desktop::UiRegistry& ui,
        services::ServiceScope& scope,
        extensions::ContributionRegistry& contributions
    )
        : impl_(std::make_unique<Impl>(
              std::move(sessions),
              std::move(opening),
              std::move(files),
              project,
              root,
              ui,
              scope,
              contributions
          ))
    {
        impl_->asset_open_ = [this](const AssetReference& reference)
        {
            if (auto queued = enqueue(reference); !queued)
            {
                log::error("content.open", "Asset open intent rejected: {}", queued.error().domain);
            }
        };
        impl_->creation_ = [this](sessions::SessionPreparation prepared
                           ) -> commands::CommandResult<commands::DispatchReceipt>
        {
            auto opened = create(std::move(prepared));
            if (!opened)
            {
                const auto& error = opened.error();
                auto code = commands::ECommandError::DOMAIN_FAILURE;
                switch (error.code)
                {
                case EEditorError::BUSY:
                    code = commands::ECommandError::BUSY;
                    break;
                case EEditorError::CLOSING:
                    code = commands::ECommandError::CLOSED;
                    break;
                case EEditorError::CAPACITY:
                    code = commands::ECommandError::CAPACITY;
                    break;
                default:
                    break;
                }
                return cxx::unexpected(commands::CommandFailure{code, error.domain, error.reason, error.message});
            }
            return commands::DispatchReceipt{
                commands::AcceptedOperation{commands::OperationKindId{"open"}, opened->value}
            };
        };
        impl_->creation_available_ =
            [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState>
        {
            if (auto admitted = impl_->admission(); !admitted)
            {
                const auto& error = admitted.error();
                const auto code = error.code == EEditorError::BUSY ? commands::ECommandError::BUSY
                                                                   : commands::ECommandError::WRONG_THREAD;
                return cxx::unexpected(commands::CommandFailure{code, error.domain, error.reason, error.message});
            }
            return commands::CommandState{hasCapacity()};
        };
    }
    ContentViews::~ContentViews() = default;
    EditorResult<sessions::OpenAssetId> ContentViews::open(AssetReference reference)
    {
        if (auto ready = impl_->admission(true); !ready)
        {
            return cxx::unexpected(ready.error());
        }
        Dispatch dispatch{impl_->dispatching_};
        return impl_->open(reference);
    }
    EditorResult<sessions::OpenAssetId> ContentViews::create(sessions::SessionPreparation data)
    {
        if (auto ready = impl_->admission(); !ready)
        {
            return cxx::unexpected(ready.error());
        }
        Dispatch dispatch{impl_->dispatching_};
        auto input = std::move(data);
        return impl_->create(std::move(input));
    }
    EditorResult<OpenAndShowResult> ContentViews::status(sessions::OpenAssetId id) const
    {
        if (auto ready = impl_->admission(); !ready)
        {
            return cxx::unexpected(ready.error());
        }
        Dispatch dispatch{impl_->dispatching_};
        const auto entry = std::ranges::find(impl_->opens_, id, &Impl::OpenPresentation::operation);
        if (entry == impl_->opens_.end())
        {
            return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "open.status"});
        }
        auto status = impl_->opening_->status(id);
        if (!status)
        {
            return failure("open.status", status.error());
        }
        return OpenAndShowResult{std::move(*status), entry->view, entry->failure};
    }
    EditorResult<void> ContentViews::cancel(sessions::OpenAssetId id)
    {
        if (auto ready = impl_->admission(); !ready)
        {
            return ready;
        }
        Dispatch dispatch{impl_->dispatching_};
        const auto entry = std::ranges::find(impl_->opens_, id, &Impl::OpenPresentation::operation);
        if (entry == impl_->opens_.end())
        {
            return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "open.cancel"});
        }
        auto cancelled = impl_->opening_->cancel(id);
        if (!cancelled)
        {
            return failure("open.cancel", cancelled.error());
        }
        entry->cancelled = true;
        return {};
    }
    EditorResult<void> ContentViews::acknowledge(sessions::OpenAssetId id)
    {
        if (auto ready = impl_->admission(); !ready)
        {
            return ready;
        }
        Dispatch dispatch{impl_->dispatching_};
        const auto entry = std::ranges::find(impl_->opens_, id, &Impl::OpenPresentation::operation);
        if (entry == impl_->opens_.end())
        {
            return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "open.acknowledge"});
        }
        auto acknowledged = impl_->opening_->acknowledge(id);
        if (!acknowledged)
        {
            return failure("open.acknowledge", acknowledged.error());
        }
        impl_->opens_.erase(entry);
        return {};
    }
    EditorResult<void> ContentViews::enqueue(AssetReference reference)
    {
        if (auto ready = impl_->admission(true); !ready)
        {
            return ready;
        }
        if (impl_->intents_.size() == 64)
        {
            return cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "open.queue"});
        }
        impl_->intents_.push_back(reference);
        return {};
    }
    EditorResult<lux::ui::PaneHandle> ContentViews::show(sessions::SessionId id, bool another)
    {
        if (auto ready = impl_->admission(true); !ready)
        {
            return cxx::unexpected(ready.error());
        }
        Dispatch dispatch{impl_->dispatching_};
        return impl_->show(id, another);
    }
    EditorResult<lux::ui::PaneHandle> ContentViews::restore(
        views::ViewContent content,
        const extensions::ContributionSnapshot& snapshot,
        views::ViewRestoreKey key,
        views::ViewTypeId type
    )
    {
        if (auto ready = impl_->admission(true); !ready)
        {
            return cxx::unexpected(ready.error());
        }
        Dispatch dispatch{impl_->dispatching_};
        return impl_->makeContentView(std::move(content), true, snapshot, std::move(key), std::move(type));
    }
    EditorResult<void> ContentViews::update()
    {
        if (auto ready = impl_->admission(); !ready)
        {
            return ready;
        }
        Dispatch dispatch{impl_->dispatching_};
        if (impl_->suspended_ || !impl_->scope_.isOpen())
        {
            return {};
        }
        EditorResult<void> result;
        const auto receive = [&](EditorFailure error)
        {
            if (result)
            {
                result = cxx::unexpected(std::move(error));
            }
        };
        for (auto i = impl_->intents_.begin(); i != impl_->intents_.end();)
        {
            auto opened = impl_->open(*i);
            if (!opened && opened.error().code == EEditorError::BUSY)
            {
                ++i;
                continue;
            }
            if (!opened)
            {
                receive(std::move(opened.error()));
            }
            i = impl_->intents_.erase(i);
        }
        for (auto& entry : impl_->opens_)
        {
            const bool is_resolved = entry.view || entry.failure || entry.cancelled;
            if (is_resolved)
            {
                continue;
            }
            auto status = impl_->opening_->status(entry.operation);
            if (!status)
            {
                const EditorResult<void> failed = failure("open.status", status.error());
                receive(failed.error());
                continue;
            }
            if (status->stage != sessions::EOpenAssetStage::PUBLISHED)
            {
                continue;
            }
            auto shown = impl_->show(status->session, false);
            if (shown)
            {
                entry.view = *shown;
            }
            else if (shown.error().code != EEditorError::BUSY)
            {
                entry.failure = std::move(shown.error());
            }
        }
        return result;
    }
    EditorResult<void> ContentViews::suspend()
    {
        if (auto ready = impl_->admission(); !ready)
        {
            return ready;
        }
        impl_->suspended_ = true;
        return {};
    }
    EditorResult<void> ContentViews::resume()
    {
        if (auto ready = impl_->admission(); !ready)
        {
            return ready;
        }
        impl_->suspended_ = false;
        return {};
    }
    bool ContentViews::hasCapacity() const noexcept
    {
        return impl_->owner_ == std::this_thread::get_id() && !impl_->dispatching_ && !impl_->suspended_ &&
               impl_->scope_.isOpen() && impl_->opens_.size() < 64;
    }
    namespace
    {
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
            {services::ServiceNameView{"lux.editor.persistence.files"},
             1,
             cxx::typeToken<persistence::IArtifactStore>(),
             services::EDependencyKind::SHARED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.project.storage"},
             1,
             cxx::typeToken<ProjectStorage>(),
             services::EDependencyKind::BORROWED,
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
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.services.scope"},
             1,
             cxx::typeToken<services::ServiceScope>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.contributions"},
             1,
             cxx::typeToken<extensions::ContributionRegistry>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT}
        };
        services::ServiceResult<std::unique_ptr<ContentViews>>
        createViews(services::ServiceResolver& resolver, const services::ServiceConfiguration&) noexcept
        {
            auto dependency0 = resolver.get<sessions::SessionStore>(0);
            if (!dependency0)
            {
                return cxx::unexpected(std::move(dependency0.error()));
            }
            auto dependency1 = resolver.get<sessions::SessionOpening>(1);
            if (!dependency1)
            {
                return cxx::unexpected(std::move(dependency1.error()));
            }
            auto dependency2 = resolver.get<persistence::IArtifactStore>(2);
            if (!dependency2)
            {
                return cxx::unexpected(std::move(dependency2.error()));
            }
            auto dependency3 = resolver.require<ProjectStorage>(3);
            if (!dependency3)
            {
                return cxx::unexpected(std::move(dependency3.error()));
            }
            auto dependency4 = resolver.require<lux::ui::Root>(4);
            if (!dependency4)
            {
                return cxx::unexpected(std::move(dependency4.error()));
            }
            auto dependency5 = resolver.require<desktop::UiRegistry>(5);
            if (!dependency5)
            {
                return cxx::unexpected(std::move(dependency5.error()));
            }
            auto dependency6 = resolver.require<services::ServiceScope>(6);
            if (!dependency6)
            {
                return cxx::unexpected(std::move(dependency6.error()));
            }
            auto dependency7 = resolver.require<extensions::ContributionRegistry>(7);
            if (!dependency7)
            {
                return cxx::unexpected(std::move(dependency7.error()));
            }
            return std::make_unique<ContentViews>(
                std::move(*dependency0),
                std::move(*dependency1),
                std::move(*dependency2),
                dependency3->get(),
                dependency4->get(),
                dependency5->get(),
                dependency6->get(),
                dependency7->get()
            );
        }
    } // namespace
    constinit const services::ServiceContract ContentViews::contracts_[]{
        services::ServiceContract::forType<ContentViews, ContentViews>(
            services::ServiceNameView{"lux.editor.project.content-views"}
        ),
        {sessions::kSessionCreation,
         1,
         cxx::typeToken<sessions::SessionCreation>(),
         [](void* value) noexcept -> void* { return &static_cast<ContentViews*>(value)->impl_->creation_; }},
        {sessions::kSessionCreationAvailability,
         1,
         cxx::typeToken<commands::CommandEntry::Query>(),
         [](void* value) noexcept -> void* { return &static_cast<ContentViews*>(value)->impl_->creation_available_; }},
        {services::ServiceNameView{"lux.editor.project.open"},
         1,
         cxx::typeToken<ProjectView::Open>(),
         [](void* value) noexcept -> void* { return &static_cast<ContentViews*>(value)->impl_->asset_open_; }}
    };
    constinit const services::ServiceDescriptor ContentViews::service = []
    {
        auto descriptor = services::ServiceDescriptor::forType<ContentViews, createViews>(
            services::ServiceNameView{"lux.editor.project.content-views"},
            contracts_,
            dependencies
        );
        descriptor.retention = services::EServiceRetention::SCOPED;
        descriptor.affinity = services::EServiceAffinity::OWNER;
        return descriptor;
    }();
} // namespace lux::editor::project
