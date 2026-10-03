#include <lux/engine/editor/storage/ProjectContentSaving.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/sessions/SessionOpening.hpp>
#include <lux/engine/editor/sessions/SessionOperations.hpp>
#include <lux/engine/editor/persistence/DerivedArtifact.hpp>
#include <lux/engine/editor/persistence/SaveService.hpp>
#include <algorithm>
#include <random>
#include <thread>

namespace lux::editor
{
    namespace
    {
        template <class Error> auto failure(std::string domain, const Error& cause)
        {
            auto code = EEditorError::SOURCE_FAILURE;
            if constexpr (requires { cause.code == decltype(cause.code)::BUSY; })
            {
                if (cause.code == decltype(cause.code)::BUSY)
                    code = EEditorError::BUSY;
            }
            else if constexpr (requires { cause == Error::BUSY; })
            {
                if (cause == Error::BUSY)
                    code = EEditorError::BUSY;
            }
            if constexpr (requires { cause.session == decltype(cause.session)::BUSY; })
                if (cause.session == decltype(cause.session)::BUSY)
                    code = EEditorError::BUSY;
            return cxx::unexpected(EditorFailure{code, std::move(domain), 0, {}, cause});
        }
    }
    ProjectSaveReport::ProjectSaveReport(persistence::SaveId value, ProjectAssetEntry entry)
        : id(value), asset(std::move(entry))
    {}
    struct ProjectContentSaving::Impl final
    {
        static constexpr std::size_t capacity_ = 128;
        sessions::SessionStore& sessions_;
        sessions::SessionOpening& opening_;
        persistence::SaveService& saves_;
        ProjectStorage& project_;
        persistence::WriteCoordinator& writes_;
        persistence::IArtifactStore& files_;
        std::vector<ProjectSaveReport> save_reports_;
        std::vector<persistence::SaveId> pending_saves_;
        std::optional<sessions::SaveAllOperation> save_all_;
        const std::thread::id owner_{std::this_thread::get_id()};
        bool dispatching_{};
        struct Dispatch final
        {
            bool& active;
            explicit Dispatch(bool& value) noexcept : active(value) { active = true; }
            ~Dispatch() { active = false; }
            Dispatch(const Dispatch&) = delete;
            Dispatch& operator=(const Dispatch&) = delete;
        };
        Impl(
            sessions::SessionStore& sessions, sessions::SessionOpening& opening,
            persistence::SaveService& saves, ProjectStorage& project,
            persistence::WriteCoordinator& writes, persistence::IArtifactStore& files
        )
            : sessions_(sessions), opening_(opening), saves_(saves), project_(project), writes_(writes), files_(files)
        {
            save_reports_.reserve(capacity_);
            pending_saves_.reserve(capacity_);
        }
        EditorResult<void> admission() const
        {
            if (owner_ != std::this_thread::get_id())
                return cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "save.owner-thread"});
            if (dispatching_)
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "save.dispatch"});
            return {};
        }
        EditorResult<void> saveAll()
        {
            auto ids = sessions_.snapshotIds();
            if (!ids)
                return failure("save-all.contents", ids.error());
            if (ids->size() > capacity_ - save_reports_.size())
                return cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "save-all.results"});
            auto admit = [this](sessions::ContentStamp content) -> sessions::SessionFactoryResult<persistence::SaveId> {
                auto saved = request(content, persistence::ESaveMode::SAVE, {});
                if (!saved)
                {
                    auto code = sessions::ESessionFactoryError::ROLE;
                    if (saved.error().code == EEditorError::BUSY)
                        code = sessions::ESessionFactoryError::BUSY;
                    return cxx::unexpected(sessions::SessionFactoryFailure{
                        code, saved.error().domain, static_cast<std::uint64_t>(saved.error().code), saved.error().message
                    });
                }
                return *saved;
            };
            auto operation = sessions::SaveAllOperation::begin(sessions_, sessions::SaveAllOperation::Request{admit});
            if (!operation)
                return failure("save-all", operation.error());
            save_all_ = std::move(*operation);
            // Every accepted source ID and its project association already have this same owner.
            return {};
        }
        EditorResult<PreparedProjectSave> prepare(
            sessions::ContentStamp content,
            persistence::ESaveMode mode,
            std::string destination
        )
        {
            auto info = sessions_.describe(content.session);
            if (!info)
                return failure("save.session", info.error());
            if (content != info->current)
                return failure("save.source", sessions::ESessionError::STALE_CONTENT);
            if (save_reports_.size() >= capacity_)
                return cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "save.reports"});
            const auto factory = opening_.factory(content.session);
            if (!factory || !(*factory)->descriptor().source)
                return cxx::unexpected(EditorFailure{EEditorError::MISSING_PROVIDER, "save.project.kind"});
            const auto& source = *(*factory)->descriptor().source;
            persistence::SaveRequest request{content.session, mode};
            request.based_on = content;
            ProjectAssetEntry entry;
            if (mode == persistence::ESaveMode::SAVE)
            {
                if (!info->binding)
                    return failure("save.unbound", persistence::EPersistenceError::UNBOUND);
                const auto* existing = project_.asset(info->binding->asset);
                if (existing)
                    entry = *existing;
                else
                {
                    // Save As may have published the source while its catalog publication failed.
                    // Recover only from the original physical binding, not a guessed current asset.
                    // Compare paths in the backend's physical key domain, including Windows case
                    // normalization and extended-path roots; logical project paths are not that domain.
                    const auto manifest_name = project_.projectFile().filename().generic_u8string();
                    auto manifest = files_.resolve(std::string_view{
                        reinterpret_cast<const char*>(manifest_name.data()), manifest_name.size()
                    });
                    if (!manifest)
                        return failure("save.binding.root", manifest.error());
                    const auto relative_path = std::filesystem::u8path(info->binding->location)
                        .lexically_relative(std::filesystem::u8path(manifest->key.value).parent_path())
                        .generic_u8string();
                    const std::string relative{
                        reinterpret_cast<const char*>(relative_path.data()), relative_path.size()
                    };
                    if (!validProjectPath(relative))
                        return cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "save.binding.path"});
                    auto physical = files_.resolve(relative);
                    if (!physical)
                        return failure("save.binding.path", physical.error());
                    if (physical->key.value != info->binding->location)
                        return cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "save.binding.identity"});
                    entry = {info->binding->asset, source.canonical_name, relative, {}, {}, {}, {}, source.version};
                    entry.mount_path = relative;
                }
            }
            else
            {
                if (!validProjectPath(destination))
                    return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "save.destination"});
                auto resolved = files_.resolve(destination);
                if (!resolved)
                    return failure("save.destination", resolved.error());
                if (resolved->expected_version != "missing")
                    return failure("save.destination.exists", persistence::EPersistenceError::CONFLICT);
                // Naming another registered source is not permission to overwrite its identity.
                for (const auto& other : project_.manifest().assets)
                {
                    auto physical = files_.resolve(other.source_path);
                    if (!physical)
                        return failure("save.catalog.target", physical.error());
                    if (physical->key == resolved->key)
                        return failure("save.destination.owned", persistence::EPersistenceError::CONFLICT);
                }
                std::mt19937 random{std::random_device{}()};
                request.asset = asset::AssetId{uuids::uuid_random_generator{random}()};
                request.destination = std::move(*resolved);
                entry = {request.asset, source.canonical_name, std::move(destination), {}, {}, {}, {}, source.version};
                entry.mount_path = entry.source_path;
            }
            return PreparedProjectSave{std::move(request), std::move(entry)};
        }
        EditorResult<persistence::SaveId> request(
            sessions::ContentStamp source,
            persistence::ESaveMode mode,
            std::string destination
        )
        {
            auto prepared = prepare(source, mode, std::move(destination));
            if (!prepared)
                return cxx::unexpected(prepared.error());
            auto accepted = saves_.requestSave(std::move(prepared->request));
            if (!accepted)
                return failure("save.admission", accepted.error());
            save_reports_.emplace_back(*accepted, std::move(prepared->asset));
            pending_saves_.push_back(*accepted); // SaveService allocated this fresh identity at the preceding admission.
            return *accepted;
        }
        EditorResult<void> track(persistence::SaveId id, std::span<const ProjectAssetEntry> destinations = {})
        {
            if (std::ranges::find(save_reports_, id, &ProjectSaveReport::id) != save_reports_.end())
                return {};
            if (save_reports_.size() >= capacity_)
                return cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "save.reports"});
            auto status = saves_.status(id);
            if (!status)
                return failure("save.status", status.error());
            // Resolve from the immutable physical destination (including a reviewed unbound close),
            // so closing or rebinding the Session cannot relabel a late disk fact.
            auto remember = [&](const auto& entries) -> EditorResult<bool> {
                for (const auto& entry : entries)
                {
                    auto target = files_.resolve(entry.source_path);
                    if (!target)
                        return failure("save.catalog.target", target.error());
                    if (target->key == status->target.key)
                    {
                        save_reports_.emplace_back(id, entry);
                        pending_saves_.push_back(id);
                        return true;
                    }
                }
                return false;
            };
            auto existing = remember(project_.manifest().assets);
            if (!existing)
                return cxx::unexpected(existing.error());
            if (*existing)
                return {};
            auto closing = remember(destinations);
            if (!closing)
                return cxx::unexpected(closing.error());
            if (*closing)
                return {};
            return cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "save.catalog.source"});
        }
        EditorResult<void> update(std::span<const sessions::SaveAllEntry> borrowed)
        {
            for (auto iterator = pending_saves_.begin(); iterator != pending_saves_.end();)
            {
                const auto id = *iterator;
                auto& report = *std::ranges::find(save_reports_, id, &ProjectSaveReport::id);
                const bool close_borrows = std::ranges::any_of(borrowed, [id](const auto& entry) {
                    return entry.save == id;
                });
                if (report.result)
                {
                    if (close_borrows)
                    {
                        ++iterator;
                        continue;
                    }
                    auto acknowledged = saves_.acknowledge(id);
                    if (!acknowledged)
                        return failure("save.acknowledge", acknowledged.error());
                    iterator = pending_saves_.erase(iterator);
                    continue;
                }
                auto status = saves_.status(id);
                if (!status)
                    return failure("save.status", status.error());
                if (status->stage != persistence::ESaveStage::TERMINAL)
                {
                    ++iterator;
                    continue;
                }
                // A terminal source observation may still describe an unknown disk publication.
                // The original SaveService keeps and refreshes that fact on reconciliation; do not
                // freeze a second outcome or acknowledge its still-owned lane here.
                const bool is_unknown = status->outcome &&
                    std::holds_alternative<persistence::PublicationUnknown>(status->outcome->publication);
                if (is_unknown)
                {
                    ++iterator;
                    continue;
                }
                const auto* published =
                    status->outcome ? std::get_if<persistence::CommitReceipt>(&status->outcome->publication) : nullptr;
                if (published && !report.failure)
                {
                    if (!report.catalog_)
                    {
                        // Preserve any newer compiled package information while applying this source publication.
                        if (const auto* existing = project_.asset(report.asset.id))
                            report.asset = *existing;
                        report.asset.source_digest = published->version;
                        ProjectUpdate update;
                        update.assets.push_back(report.asset);
                        auto candidate = project_.preparePublication(update);
                        if (!candidate)
                        {
                            if (candidate.error().code == EEditorError::BUSY)
                            {
                                ++iterator;
                                continue;
                            }
                            report.failure = candidate.error();
                        }
                        else
                        {
                            auto target = files_.resolve(candidate->plan().manifestPath());
                            if (!target)
                                report.failure = failure("catalog.target", target.error()).value();
                            else
                            {
                                target->expected_version = candidate->plan().beforeManifestDigest();
                                auto ticket = persistence::publishEncodedArtifact(
                                    writes_, std::move(*target),
                                    persistence::EncodedArtifact{candidate->plan().manifestBytes()}
                                );
                                if (!ticket)
                                    report.failure = failure("catalog.publish", ticket.error()).value();
                                else
                                {
                                    report.catalog_ = std::move(*candidate);
                                    report.catalog_ticket = *ticket;
                                }
                            }
                        }
                    }
                    if (report.catalog_ticket)
                    {
                        auto written = writes_.status(*report.catalog_ticket);
                        if (!written)
                            return failure("catalog.status", written.error());
                        // Unknown is still a live lane and still owns the Project reservation.
                        if (written->stage != persistence::EWriteStage::TERMINAL)
                        {
                            ++iterator;
                            continue;
                        }
                        if (auto* receipt = std::get_if<persistence::CommitReceipt>(&*written->outcome))
                        {
                            ProjectPublicationReceipt adopted{
                                report.catalog_->plan().manifest(),
                                receipt->version,
                                1,
                                {},
                                {{report.asset.source_path, published->version}},
                                {},
                                report.catalog_->sharePlan()
                            };
                            auto result = project_.adoptPublication(*report.catalog_, adopted);
                            if (!result)
                                report.failure = std::move(result.error());
                        }
                        else
                            report.failure = failure("catalog.publication", *written->outcome).value();
                        auto acknowledged = writes_.acknowledge(*report.catalog_ticket);
                        if (!acknowledged)
                            return failure("catalog.acknowledge", acknowledged.error());
                        report.catalog_ticket.reset();
                        report.catalog_.reset();
                    }
                }
                report.result = status->outcome;
                if (close_borrows)
                {
                    ++iterator;
                    continue;
                }
                auto acknowledged = saves_.acknowledge(id);
                if (!acknowledged)
                    return failure("save.acknowledge", acknowledged.error());
                iterator = pending_saves_.erase(iterator);
            }
            return {};
        }
    };
    ProjectContentSaving::ProjectContentSaving(
        sessions::SessionStore& sessions, sessions::SessionOpening& opening, persistence::SaveService& saves,
        ProjectStorage& project, persistence::WriteCoordinator& writes, persistence::IArtifactStore& files
    ) : impl_(std::make_unique<Impl>(sessions, opening, saves, project, writes, files))
    {}
    ProjectContentSaving::~ProjectContentSaving() = default;
    EditorResult<PreparedProjectSave> ProjectContentSaving::prepare(
        sessions::ContentStamp source, persistence::ESaveMode mode, std::string destination
    )
    {
        if (auto admitted = impl_->admission(); !admitted)
            return cxx::unexpected(admitted.error());
        Impl::Dispatch scope{impl_->dispatching_};
        return impl_->prepare(source, mode, std::move(destination));
    }
    EditorResult<persistence::SaveId> ProjectContentSaving::request(
        sessions::ContentStamp source, persistence::ESaveMode mode, std::string destination
    )
    {
        if (auto admitted = impl_->admission(); !admitted)
            return cxx::unexpected(admitted.error());
        Impl::Dispatch scope{impl_->dispatching_};
        return impl_->request(source, mode, std::move(destination));
    }
    EditorResult<void> ProjectContentSaving::track(persistence::SaveId id, std::span<const ProjectAssetEntry> destinations)
    {
        if (auto admitted = impl_->admission(); !admitted)
            return cxx::unexpected(admitted.error());
        Impl::Dispatch scope{impl_->dispatching_};
        return impl_->track(id, destinations);
    }
    EditorResult<void> ProjectContentSaving::update(std::span<const sessions::SaveAllEntry> borrowed)
    {
        if (auto admitted = impl_->admission(); !admitted)
            return cxx::unexpected(admitted.error());
        Impl::Dispatch scope{impl_->dispatching_};
        return impl_->update(borrowed);
    }
    EditorResult<void> ProjectContentSaving::saveAll()
    {
        if (auto admitted = impl_->admission(); !admitted)
            return cxx::unexpected(admitted.error());
        Impl::Dispatch scope{impl_->dispatching_};
        return impl_->saveAll();
    }
    EditorResult<void> ProjectContentSaving::acknowledge(persistence::SaveId id)
    {
        if (auto admitted = impl_->admission(); !admitted)
            return admitted;
        Impl::Dispatch scope{impl_->dispatching_};
        auto found = std::ranges::find(impl_->save_reports_, id, &ProjectSaveReport::id);
        if (found == impl_->save_reports_.end())
            return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "save.result"});
        const bool is_pending = std::ranges::find(impl_->pending_saves_, id) != impl_->pending_saves_.end();
        if (!found->result || is_pending)
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "save.result"});
        impl_->save_reports_.erase(found);
        return {};
    }
    EditorResult<void> ProjectContentSaving::acknowledgeSaveAll()
    {
        if (auto admitted = impl_->admission(); !admitted)
            return admitted;
        Impl::Dispatch scope{impl_->dispatching_};
        impl_->save_all_.reset();
        return {};
    }
    std::span<const ProjectSaveReport> ProjectContentSaving::reports() const noexcept { return impl_->save_reports_; }
    std::span<const persistence::SaveId> ProjectContentSaving::pending() const noexcept { return impl_->pending_saves_; }
    std::span<const sessions::SaveAllEntry> ProjectContentSaving::saveAllEntries() const noexcept
    {
        return impl_->save_all_ ? impl_->save_all_->entries() : std::span<const sessions::SaveAllEntry>{};
    }
    bool ProjectContentSaving::hasSaveAll() const noexcept { return impl_->save_all_.has_value(); }
    bool ProjectContentSaving::hasCapacity(std::size_t count) const noexcept
    {
        return count <= Impl::capacity_ - impl_->save_reports_.size();
    }
    bool ProjectContentSaving::settled() const noexcept
    {
        return impl_->pending_saves_.empty();
    }
}
