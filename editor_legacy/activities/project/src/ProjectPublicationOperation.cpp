#include <lux/engine/editor/storage/ProjectPublicationOperation.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/persistence/SaveExecution.hpp>
#include <lux/engine/log/Log.hpp>

namespace lux::editor
{
    using namespace persistence;
    namespace
    {
        template <class T> EditorFailure publicationFailure(std::string domain, T error)
        {
            return {EEditorError::SOURCE_FAILURE, std::move(domain), 0, {}, std::move(error)};
        }
    }
    struct ProjectPublicationOperation::Impl final
    {
        enum class EStage
        {
            FILES,
            PACKAGES,
            MANIFEST,
            ADOPT
        };
        struct Prepared final
        {
            std::vector<ProjectPackage> packages;
        };
        ProjectStorage& project_;
        WriteCoordinator& writes_;
        IArtifactStore& files_;
        SaveExecution& execution_;
        PreparedProjectPublication publication_;
        ProjectPublicationReceipt receipt_;
        VPublicationStatus status_;
        EStage stage_{EStage::FILES};
        std::size_t next_file_{};
        std::optional<WriteTicket> ticket_;
        std::optional<EditorResult<Prepared>> prepared_;
        bool reading_{}, abandoning_{}, dispatching_{};
        process::TaskScope tasks_;

        Impl(
            ProjectStorage& project,
            process::ExecutionRuntime& runtime,
            WriteCoordinator& writes,
            IArtifactStore& files,
            SaveExecution& execution,
            PreparedProjectPublication publication
        )
            : project_(project), writes_(writes), files_(files), execution_(execution),
              publication_(std::move(publication)), tasks_(runtime)
        {
            if (!publication_.sharePlan())
            {
                status_ = EditorFailure{EEditorError::INVALID_ARGUMENT, "project.publication.plan"};
                return;
            }
            receipt_.manifest = publication_.plan().manifest();
            receipt_.plan = publication_.sharePlan();
            // A failed manifest must leave the old project's visible sources intact.
            // Mutable author saves use SaveService, not this immutable package transaction.
            if (auto valid = validateInput(); !valid)
                status_ = valid.error();
        }
        EditorResult<void> validateInput() const
        {
            if (!publication_.sharePlan())
                return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "project.publication.plan"});
            for (const auto& file : publication_.plan().files())
            {
                if (file.before_digest != "missing")
                    return cxx::unexpected(publicationFailure(
                        "project.publication.mutable-input",
                        PersistenceFailure{EPersistenceError::INVALID_ARGUMENT, file.path}
                    ));
            }
            return {};
        }
        bool terminal() const noexcept
        {
            return std::holds_alternative<PublicationSucceeded>(status_) ||
                   std::holds_alternative<PublicationAbandoned>(status_);
        }
        void finishAbandon()
        {
            if (ticket_ || reading_)
                return;
            publication_ = {};
            status_ = PublicationAbandoned{receipt_.published_files};
        }
        void update()
        {
            if (dispatching_ || terminal())
                return;
            struct Dispatch final
            {
                bool& value;
                explicit Dispatch(bool& active) : value(active)
                {
                    value = true;
                }
                ~Dispatch()
                {
                    value = false;
                }
            } dispatch{dispatching_};
            if (ticket_)
            {
                auto state = writes_.status(*ticket_);
                if (!state)
                {
                    status_ = publicationFailure("project.publication.status", state.error());
                    return;
                }
                if (abandoning_ && (state->stage == EWriteStage::READY || state->stage == EWriteStage::RESERVED))
                {
                    auto cancelled = writes_.cancelBeforePublish(*ticket_, {EPersistenceError::CANCELLED});
                    if (!cancelled)
                        status_ = publicationFailure("project.publication.cancel", cancelled.error());
                    return;
                }
                if (state->stage == EWriteStage::UNKNOWN)
                {
                    if (!std::holds_alternative<EditorFailure>(status_))
                        status_ = publicationFailure("project.publication.unknown", *state->outcome);
                    return; // Explicit reconciliation retains the lane, bytes and project reservation.
                }
                if (state->stage != EWriteStage::TERMINAL)
                    return;
                // status() owns its outcome; acknowledgement cannot invalidate this local fact.
                auto acknowledged = writes_.acknowledge(*ticket_);
                if (!acknowledged)
                {
                    status_ = publicationFailure("project.publication.acknowledge", acknowledged.error());
                    return;
                }
                ticket_.reset();
                const auto published = std::get_if<CommitReceipt>(&*state->outcome);
                if (published)
                {
                    ++receipt_.published_files;
                    if (published->warning)
                        receipt_.cleanup =
                            cxx::unexpected(publicationFailure("project.publication.durability", *published->warning));
                    if (stage_ == EStage::MANIFEST)
                    {
                        receipt_.manifest_digest = published->version;
                        stage_ = EStage::ADOPT; // Cancellation cannot erase this publication fact.
                    }
                    else
                    {
                        receipt_.file_digests.emplace_back(publication_.plan().files()[next_file_].path, published->version);
                        ++next_file_;
                    }
                }
                else
                    status_ = publicationFailure("project.publication.write", *state->outcome);
            }
            if (stage_ == EStage::ADOPT)
            {
                // This is a retryable adoption of the original receipt, never a second publication.
                auto adopted = project_.adoptPublication(publication_, receipt_);
                if (!adopted)
                {
                    status_ = std::move(adopted.error());
                    return;
                }
                publication_ = {};
                status_ = PublicationSucceeded{std::move(receipt_.cleanup)};
                return;
            }
            if (abandoning_)
            {
                finishAbandon();
                return;
            }
            if (std::holds_alternative<EditorFailure>(status_))
                return;
            if (stage_ == EStage::FILES)
            {
                if (next_file_ < publication_.plan().files().size())
                {
                    const auto& file = publication_.plan().files()[next_file_];
                    auto target = files_.resolve(file.path);
                    if (!target)
                    {
                        status_ = publicationFailure("project.publication.target", target.error());
                        return;
                    }
                    const auto digest = projectContentDigest(file.bytes.view());
                    if (file.reuse_identical && target->expected_version == digest)
                    {
                        receipt_.file_digests.emplace_back(file.path, digest);
                        ++next_file_;
                        return;
                    }
                    // Do not inherit an unrelated writer's current version.
                    target->expected_version = file.before_digest;
                    auto written = publishEncodedArtifact(writes_, std::move(*target), EncodedArtifact{file.bytes});
                    if (!written)
                        status_ = publicationFailure("project.publication.admission", written.error());
                    else
                        ticket_ = *written;
                    return;
                }
                stage_ = EStage::PACKAGES;
            }
            if (stage_ == EStage::PACKAGES)
            {
                if (reading_)
                    return;
                if (!prepared_)
                {
                    auto blocking = tasks_.execution().blocking();
                    if (!blocking)
                    {
                        status_ = publicationFailure("project.publication.scheduler", blocking.error());
                        return;
                    }
                    reading_ = true;
                    auto accepted = tasks_.submit(
                        {"Prepare project catalog", "Storage"},
                        [scheduler = *blocking,
                         plan = publication_.sharePlan()](process::TaskReporter) mutable noexcept {
                            return stdexec::then(
                                stdexec::schedule(scheduler),
                                [plan = std::move(plan)](
                                ) -> EditorResult<Prepared> {
                                    Prepared result;
                                    for (const auto& path : plan->packagePaths())
                                    {
                                        auto package = readProjectPackage(plan->root(), path);
                                        if (!package)
                                            return cxx::unexpected(package.error());
                                        result.packages.push_back(std::move(*package));
                                    }
                                    return result;
                                }
                            );
                        },
                        [this](process::TTaskResult<Prepared, EditorFailure>&& result) noexcept {
                            reading_ = false; // Accepted completion is retained even while cancelling/dispatching.
                            if (result)
                                prepared_.emplace(std::move(*result));
                            else if (auto* error = result.error().domainFailure())
                                prepared_.emplace(cxx::unexpected(std::move(*error)));
                            else
                                prepared_.emplace(
                                    cxx::unexpected(EditorFailure{EEditorError::EXECUTION_FAILURE, "project.prepare"})
                                );
                        }
                    );
                    if (!accepted)
                    {
                        reading_ = false;
                        status_ = publicationFailure("project.publication.submit", accepted.error());
                    }
                    return;
                }
                if (!*prepared_)
                {
                    status_ = prepared_->error();
                    return;
                }
                receipt_.packages = std::move((**prepared_).packages);
                stage_ = EStage::MANIFEST;
            }
            auto target = files_.resolve(publication_.plan().manifestPath());
            if (!target)
            {
                status_ = publicationFailure("project.manifest.target", target.error());
                return;
            }
            target->expected_version = publication_.plan().beforeManifestDigest();
            auto written =
                publishEncodedArtifact(writes_, std::move(*target), EncodedArtifact{publication_.plan().manifestBytes()});
            if (!written)
                status_ = publicationFailure("project.manifest.admission", written.error());
            else
                ticket_ = *written;
        }
        EditorResult<void> retry()
        {
            if (dispatching_ || !std::holds_alternative<EditorFailure>(status_))
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "project.publication.retry"});
            if (auto valid = validateInput(); !valid)
                return valid;
            if (ticket_)
            {
                auto state = writes_.status(*ticket_);
                if (!state)
                    return cxx::unexpected(publicationFailure("project.publication.status", state.error()));
                if (state->stage == EWriteStage::UNKNOWN)
                {
                    auto reconciled = writes_.reconcile(*ticket_, files_);
                    if (!reconciled)
                        return cxx::unexpected(publicationFailure("project.publication.reconcile", reconciled.error()));
                }
            }
            if (prepared_ && !*prepared_)
                prepared_.reset();
            status_ = PublicationPending{};
            return {};
        }
        void abandon()
        {
            abandoning_ = true;
            tasks_.requestStop();
        }
        ~Impl()
        {
            abandon();
            if (!tasks_.join())
                std::terminate();
            // Normal application retirement reaches terminal first. RAII also drains an interrupted
            // local owner while the shared execution/coordinator/store are still alive.
            auto drained = tasks_.execution().waitUntil([this]() noexcept {
                update();
                if (terminal())
                    return true;
                if (ticket_)
                {
                    auto state = writes_.status(*ticket_);
                    if (!state)
                        std::terminate();
                    if (state->stage == EWriteStage::UNKNOWN)
                    {
                        // One real file reconciliation; never release an unresolved writer.
                        auto reconciled = writes_.reconcile(*ticket_, files_);
                        if (!reconciled)
                            std::terminate();
                        update();
                        if (terminal())
                            return true;
                        if (ticket_)
                        {
                            state = writes_.status(*ticket_);
                            if (state && state->stage == EWriteStage::UNKNOWN)
                                std::terminate();
                        }
                    }
                }
                if (!execution_.submitReady())
                    std::terminate();
                // No IO wake is expected for a local continuation or failed adoption.
                if (!reading_ && !ticket_)
                {
                    if (stage_ == EStage::ADOPT && std::holds_alternative<EditorFailure>(status_))
                        std::terminate();
                    tasks_.execution().wake();
                }
                return false;
            });
            if (!drained)
                std::terminate();
        }
    };
    ProjectPublicationOperation::ProjectPublicationOperation(
        ProjectStorage& project,
        process::ExecutionRuntime& runtime,
        WriteCoordinator& writes,
        IArtifactStore& files,
        SaveExecution& execution,
        PreparedProjectPublication publication
    )
        : impl_(std::make_unique<Impl>(project, runtime, writes, files, execution, std::move(publication)))
    {}
    ProjectPublicationOperation::~ProjectPublicationOperation() = default;
    void ProjectPublicationOperation::update()
    {
        impl_->update();
    }
    const VPublicationStatus& ProjectPublicationOperation::status() const noexcept
    {
        return impl_->status_;
    }
    std::optional<WriteTicket> ProjectPublicationOperation::ticket() const noexcept
    {
        return impl_->ticket_;
    }
    bool ProjectPublicationOperation::terminal() const noexcept
    {
        return impl_->terminal();
    }
    EditorResult<void> ProjectPublicationOperation::retry()
    {
        return impl_->retry();
    }
    void ProjectPublicationOperation::abandon()
    {
        impl_->abandon();
    }
}
