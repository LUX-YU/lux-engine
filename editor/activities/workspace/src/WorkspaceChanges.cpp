#include <lux/engine/editor/workspace/WorkspaceChanges.hpp>
#include <algorithm>
#include <thread>

namespace lux::editor::workspace
{
    namespace
    {
        template<class Error> auto failure(std::string domain, const Error& cause)
        {
            auto code = EEditorError::SOURCE_FAILURE;
            if constexpr (requires { cause.code == decltype(cause.code)::BUSY; })
                if (cause.code == decltype(cause.code)::BUSY)
                    code = EEditorError::BUSY;
            return cxx::unexpected(EditorFailure{code, std::move(domain), 0, {}, cause});
        }
    }
    struct WorkspaceChanges::Impl final
    {
        static constexpr std::size_t capacity = 16;
        WorkspaceStore& store_;
        persistence::WriteCoordinator& writes_;
        persistence::IArtifactStore& files_;
        const std::thread::id owner_{std::this_thread::get_id()};
        bool dispatching_{};
        LayoutCatalog catalog_;
        std::vector<WorkspacePublication> publications_;
        std::optional<LegacyMigration> migration_;
        std::optional<persistence::WriteTicket> migration_ticket_;
        std::optional<EditorFailure> migration_failure_;
        bool migration_complete_{};

        Impl(WorkspaceStore& store, persistence::WriteCoordinator& writes, persistence::IArtifactStore& files)
            : store_(store), writes_(writes), files_(files)
        {
            publications_.reserve(capacity);
        }
        struct Dispatch final
        {
            bool& active;
            explicit Dispatch(bool& value) noexcept : active(value) { active = true; }
            ~Dispatch() { active = false; }
            Dispatch(const Dispatch&) = delete;
            Dispatch& operator=(const Dispatch&) = delete;
        };
        EditorResult<void> admission(bool requires_capacity = false) const
        {
            if (owner_ != std::this_thread::get_id())
                return cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "workspace.owner-thread"});
            if (dispatching_)
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "workspace.dispatch"});
            if (requires_capacity && publications_.size() == capacity)
                return cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "workspace.results"});
            return {};
        }
        template<class Write> EditorResult<persistence::WriteTicket>
        accept(std::string label, Write&& write, bool refresh_catalog = true)
        {
            if (auto ready = admission(true); !ready)
                return cxx::unexpected(ready.error());
            const Dispatch scope{dispatching_};
            auto accepted = write();
            if (!accepted)
                return failure("workspace.publication", accepted.error());
            publications_.push_back({std::move(label), *accepted, {}, {}, refresh_catalog});
            return *accepted;
        }
        template<class Write> EditorResult<void> publish(std::string label, Write&& write)
        {
            auto accepted = accept(std::move(label), std::forward<Write>(write));
            if (!accepted)
                return cxx::unexpected(accepted.error());
            return {};
        }
        EditorResult<void> refresh()
        {
            if (auto ready = admission(); !ready)
                return ready;
            const Dispatch scope{dispatching_};
            auto catalog = store_.listLayouts();
            if (!catalog)
                return failure("workspace.catalog", catalog.error());
            catalog_ = std::move(*catalog);
            return {};
        }
        EditorResult<void> select(const LayoutId& id)
        {
            if (auto ready = admission(true); !ready)
                return ready;
            const Dispatch scope{dispatching_};
            auto previous = store_.readPreferences();
            if (!previous && previous.error().code != EWorkspaceError::NOT_FOUND)
                return failure("workspace.applied.preferences-read", previous.error());
            auto preferences = previous ? std::move(previous->value) : UserPreferences{};
            const auto version = previous ? previous->target.expected_version : "missing";
            preferences.selected_layout = id;
            auto accepted = store_.writePreferences(preferences, version);
            if (!accepted)
                return failure("workspace.publication", accepted.error());
            publications_.push_back({"Applied layout; persist selection", *accepted});
            return {};
        }
        EditorResult<void> migrate()
        {
            if (auto ready = admission(true); !ready)
                return ready;
            const Dispatch scope{dispatching_};
            if (migration_ticket_)
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "workspace.migration.pending"});
            auto input = store_.prepareLegacyMigration();
            if (!input)
                return failure("workspace.migration.read", input.error());
            migration_ = std::move(*input);
            migration_failure_.reset();
            migration_complete_ = false;
            return {};
        }
        EditorResult<void> reconcile(persistence::WriteTicket ticket)
        {
            if (auto ready = admission(); !ready)
                return ready;
            const Dispatch scope{dispatching_};
            const auto found = std::ranges::find(publications_, ticket, &WorkspacePublication::ticket);
            if (found == publications_.end())
                return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "workspace.result"});
            auto reconciled = writes_.reconcile(ticket, files_);
            return reconciled ? EditorResult<void>{} : failure("workspace.reconcile", reconciled.error());
        }
        EditorResult<void> acknowledge(persistence::WriteTicket ticket)
        {
            if (auto ready = admission(); !ready)
                return ready;
            const Dispatch scope{dispatching_};
            const auto found = std::ranges::find(publications_, ticket, &WorkspacePublication::ticket);
            if (found == publications_.end())
                return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "workspace.result"});
            const bool is_pending = !found->result || migration_ticket_ == ticket;
            if (is_pending)
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "workspace.result"});
            publications_.erase(found);
            return {};
        }
        void advanceMigration(bool allow_new_work)
        {
            const bool has_work = migration_ && !migration_complete_ && !migration_failure_;
            if (!has_work)
                return;
            if (migration_ticket_)
            {
                const auto found = std::ranges::find(publications_, *migration_ticket_, &WorkspacePublication::ticket);
                if (found == publications_.end() || !found->result)
                    return;
                if (!std::holds_alternative<persistence::CommitReceipt>(*found->result))
                {
                    migration_failure_ = failure("workspace.migration.publication", *found->result).value();
                    migration_ticket_.reset();
                    return;
                }
                migration_ticket_.reset();
            }
            const bool can_start = allow_new_work && publications_.size() < capacity;
            if (!can_start)
                return;
            auto next = store_.continueMigration(*migration_);
            if (!next)
            {
                if (next.error().code != EWorkspaceError::BUSY)
                    migration_failure_ = failure("workspace.migration", next.error()).value();
                return;
            }
            if (!*next)
                migration_complete_ = true;
            else
            {
                migration_ticket_ = **next;
                publications_.push_back({"Migrate one legacy record", **next});
            }
        }
        EditorResult<void> update(bool allow_new_work)
        {
            if (auto ready = admission(); !ready)
                return ready;
            const Dispatch scope{dispatching_};
            for (auto& report : publications_)
            {
                if (report.result)
                    continue;
                auto status = writes_.status(report.ticket);
                if (!status)
                    return failure("workspace.status", status.error());
                if (status->stage != persistence::EWriteStage::TERMINAL)
                    continue;
                auto acknowledged = writes_.acknowledge(report.ticket);
                if (!acknowledged)
                    return failure("workspace.acknowledge", acknowledged.error());
                report.result = std::move(status->outcome);
                if (!report.refresh_catalog)
                    continue;
                auto catalog = store_.listLayouts();
                if (catalog)
                    catalog_ = std::move(*catalog);
                else
                    report.catalog_failure = catalog.error();
            }
            advanceMigration(allow_new_work);
            return {};
        }
    };
    WorkspaceChanges::WorkspaceChanges(
        WorkspaceStore& store, persistence::WriteCoordinator& writes, persistence::IArtifactStore& files
    ) : impl_(std::make_unique<Impl>(store, writes, files))
    {}
    WorkspaceChanges::~WorkspaceChanges() = default;
    EditorResult<void> WorkspaceChanges::refresh() { return impl_->refresh(); }
    EditorResult<void> WorkspaceChanges::save(const DockLayout& layout)
    {
        return impl_->publish("Save layout: " + layout.label, [&] {
            return impl_->store_.saveLayout(layout, "missing");
        });
    }
    EditorResult<void> WorkspaceChanges::rename(const LayoutId& id, std::string label)
    {
        return impl_->publish("Rename layout: " + label, [&] {
            return impl_->store_.renameLayout(id, std::move(label));
        });
    }
    EditorResult<void> WorkspaceChanges::remove(const LayoutId& id)
    {
        return impl_->publish("Delete layout: " + id.value, [&] { return impl_->store_.removeLayout(id); });
    }
    EditorResult<void> WorkspaceChanges::select(const LayoutId& id) { return impl_->select(id); }
    EditorResult<void> WorkspaceChanges::recordRecovery(const RecoveryManifest& value, std::string version)
    {
        return impl_->publish("Record recovery locations (not unsaved content)", [&] {
            return impl_->store_.writeRecovery(value, std::move(version));
        });
    }
    EditorResult<void> WorkspaceChanges::migrate() { return impl_->migrate(); }
    EditorResult<persistence::WriteTicket>
    WorkspaceChanges::saveSettings(std::string_view relative, const settings::SettingsDocument& value)
    {
        return impl_->accept("Save settings: " + std::string(relative), [&] {
            return impl_->store_.writeSettings(relative, value);
        }, false);
    }
    EditorResult<void> WorkspaceChanges::reconcile(persistence::WriteTicket ticket) { return impl_->reconcile(ticket); }
    EditorResult<void> WorkspaceChanges::acknowledge(persistence::WriteTicket ticket)
    {
        return impl_->acknowledge(ticket);
    }
    EditorResult<void> WorkspaceChanges::update(bool allow_new_work) { return impl_->update(allow_new_work); }
    bool WorkspaceChanges::hasCapacity() const noexcept { return impl_->publications_.size() < Impl::capacity; }
    bool WorkspaceChanges::settled() const noexcept
    {
        return std::ranges::all_of(impl_->publications_, [](const auto& report) { return report.result.has_value(); });
    }
    const LayoutCatalog& WorkspaceChanges::catalog() const noexcept { return impl_->catalog_; }
    std::span<const WorkspacePublication> WorkspaceChanges::publications() const noexcept
    {
        return impl_->publications_;
    }
    const LegacyMigration* WorkspaceChanges::migration() const noexcept
    {
        return impl_->migration_ ? &*impl_->migration_ : nullptr;
    }
    const EditorFailure* WorkspaceChanges::migrationFailure() const noexcept
    {
        return impl_->migration_failure_ ? &*impl_->migration_failure_ : nullptr;
    }
    bool WorkspaceChanges::migrationComplete() const noexcept { return impl_->migration_complete_; }
}
