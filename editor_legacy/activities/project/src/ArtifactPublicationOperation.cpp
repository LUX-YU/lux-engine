#include <lux/engine/editor/storage/ArtifactPublicationOperation.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/persistence/DerivedArtifact.hpp>
#include <lux/engine/editor/persistence/SaveExecution.hpp>
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <random>

namespace lux::editor
{
    namespace
    {
        template<class Error> EditorFailure failure(std::string domain, Error error)
        {
            return {EEditorError::SOURCE_FAILURE, std::move(domain), 0, {}, std::move(error)};
        }
    }
    struct ArtifactPublicationOperation::Impl final
    {
        struct Package final
        {
            cxx::SharedBytes<> bytes;
            std::string source_digest;
        };
        ProjectStorage& project_;
        persistence::WriteCoordinator& writes_;
        persistence::IArtifactStore& files_;
        persistence::SaveExecution& execution_;
        persistence::DerivedArtifact source_;
        ProjectAssetEntry asset_;
        std::unique_ptr<ProjectPublicationOperation> publication_;
        std::optional<EditorResult<Package>> package_;
        VPublicationStatus status_;
        bool encoding_{}, abandoning_{}, dispatching_{};
        process::TaskScope tasks_;

        Impl(ProjectStorage& project, process::ExecutionRuntime& runtime,
            persistence::WriteCoordinator& writes, persistence::IArtifactStore& files,
            persistence::SaveExecution& execution, persistence::DerivedArtifact source, ProjectAssetEntry asset)
            : project_(project), writes_(writes), files_(files), execution_(execution), source_(std::move(source)),
              asset_(std::move(asset)), tasks_(runtime)
        {
            std::mt19937 random{std::random_device{}()};
            asset_.cooked_path = ".lux/compiled/" + uuids::to_string(asset_.id.uuid()) + "/" +
                uuids::to_string(uuids::uuid_random_generator{random}()) + ".pak";
        }
        void update()
        {
            if (dispatching_)
                return;
            struct Dispatch final
            {
                bool& active;
                explicit Dispatch(bool& value) : active(value) { active = true; }
                ~Dispatch() { active = false; }
            } dispatch{dispatching_};
            if (publication_)
            {
                publication_->update();
                return;
            }
            if (abandoning_)
            {
                if (!encoding_)
                    status_ = PublicationAbandoned{};
                return;
            }
            if (std::holds_alternative<EditorFailure>(status_))
                return;
            if (!package_)
            {
                if (encoding_)
                    return;
                encoding_ = true;
                auto submitted = tasks_.submit(
                    {"Package compiled asset", "Compiler"},
                    [cpu = tasks_.execution().cpu(), source = source_](process::TaskReporter reporter) mutable noexcept {
                        return stdexec::then(stdexec::schedule(cpu),
                            [source = std::move(source), reporter]() -> EditorResult<Package> {
                                if (reporter.stopToken().stop_requested())
                                    return cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "artifact.encode"});
                                auto encoded_source = source.encodeSource(reporter.stopToken());
                                if (!encoded_source)
                                    return cxx::unexpected(failure("artifact.source.encode", encoded_source.error()));
                                const auto digest = projectContentDigest(encoded_source->bytes.view());
                                const auto& info = source.info();
                                auto bytes = asset::encodePak(
                                    {{info.source_asset, info.primary_magic, uuids::to_string(info.source_asset.uuid()),
                                      {}, source.bytes()}}, 256U * 1024U * 1024U, "/Project"
                                );
                                if (!bytes)
                                    return cxx::unexpected(failure("artifact.package.encode", bytes.error()));
                                persistence::EncodedArtifact frozen{std::move(*bytes)};
                                return Package{std::move(frozen.bytes), digest};
                            }
                        );
                    },
                    [this](process::TTaskResult<Package, EditorFailure>&& result) noexcept {
                        // Completion is an already accepted fact, including during outer dispatch/stop.
                        encoding_ = false;
                        if (result)
                            package_.emplace(std::move(*result));
                        else if (auto* error = result.error().domainFailure())
                            package_.emplace(cxx::unexpected(std::move(*error)));
                        else
                            package_.emplace(cxx::unexpected(EditorFailure{
                                result.error().isCancelled() ? EEditorError::CANCELLED : EEditorError::EXECUTION_FAILURE,
                                "artifact.encode"
                            }));
                    }
                );
                if (!submitted)
                {
                    encoding_ = false;
                    status_ = failure("artifact.submit", submitted.error());
                }
                return;
            }
            if (!*package_)
            {
                status_ = package_->error();
                return;
            }
            // Capture the current source catalog only after encoding. A later source save keeps
            // its own original publication lane and is not mislabeled as this frozen compilation.
            const auto* current = project_.asset(asset_.id);
            if (!current)
            {
                status_ = EditorFailure{EEditorError::STALE_REQUEST, "artifact.catalog.source"};
                return;
            }
            auto asset = *current;
            asset.cooked_path = asset_.cooked_path;
            asset.compiled_source_digest = (**package_).source_digest;
            ProjectUpdate update;
            update.assets.push_back(std::move(asset));
            update.files.push_back({asset_.cooked_path, "missing", (**package_).bytes});
            auto prepared = project_.preparePublication(update);
            if (!prepared)
            {
                if (prepared.error().code != EEditorError::BUSY)
                    status_ = prepared.error();
                return;
            }
            publication_ = std::make_unique<ProjectPublicationOperation>(project_, tasks_.execution(), writes_,
                files_, execution_, std::move(*prepared));
        }
        EditorResult<void> retry()
        {
            if (dispatching_ || abandoning_)
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "artifact.retry"});
            if (publication_)
                return publication_->retry();
            if (!std::holds_alternative<EditorFailure>(status_))
                return cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "artifact.retry"});
            if (package_ && !*package_)
                package_.reset();
            status_ = PublicationPending{};
            return {};
        }
        void abandon()
        {
            abandoning_ = true;
            tasks_.requestStop();
            if (publication_)
                publication_->abandon();
        }
        ~Impl()
        {
            abandon();
            if (!tasks_.join())
                std::terminate();
            // The nested original publication owner drains its actual receipts before releasing
            // its reservation. These shared services still outlive this whole operation.
        }
    };
    ArtifactPublicationOperation::CreateResult ArtifactPublicationOperation::create(
        persistence::DerivedArtifact& source, sessions::SessionStore& sessions, ProjectStorage& project,
        process::ExecutionRuntime& runtime, persistence::WriteCoordinator& writes,
        persistence::IArtifactStore& files, persistence::SaveExecution& execution
    )
    {
        if (!source.valid())
            return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "artifact.input"});
        auto current = sessions.describe(source.info().content.session);
        if (!current)
            return cxx::unexpected(EditorFailure{
                current.error() == sessions::ESessionError::BUSY ? EEditorError::BUSY : EEditorError::STALE_REQUEST,
                "artifact.source", 0, {}, current.error()
            });
        if (current->current != source.info().content)
            return cxx::unexpected(failure("artifact.source", sessions::ESessionError::STALE_CONTENT));
        if (!current->binding)
            return cxx::unexpected(failure("artifact.binding", persistence::EPersistenceError::UNBOUND));
        const auto* asset = project.asset(current->binding->asset);
        const bool is_invalid_asset = !asset || source.info().source_asset != asset->id;
        if (is_invalid_asset)
            return cxx::unexpected(failure("artifact.binding", persistence::EPersistenceError::STALE_SOURCE));
        auto impl = std::make_unique<Impl>(project, runtime, writes, files, execution, std::move(source), *asset);
        return std::unique_ptr<ArtifactPublicationOperation>(new ArtifactPublicationOperation(std::move(impl)));
    }
    ArtifactPublicationOperation::ArtifactPublicationOperation(std::unique_ptr<Impl> impl) noexcept
        : impl_(std::move(impl)) {}
    ArtifactPublicationOperation::~ArtifactPublicationOperation() = default;
    void ArtifactPublicationOperation::update() { impl_->update(); }
    const VPublicationStatus& ArtifactPublicationOperation::status() const noexcept
    {
        return impl_->publication_ ? impl_->publication_->status() : impl_->status_;
    }
    std::optional<persistence::WriteTicket> ArtifactPublicationOperation::ticket() const noexcept
    {
        return impl_->publication_ ? impl_->publication_->ticket() : std::nullopt;
    }
    std::string_view ArtifactPublicationOperation::path() const noexcept { return impl_->asset_.cooked_path; }
    bool ArtifactPublicationOperation::terminal() const noexcept
    {
        return std::holds_alternative<PublicationSucceeded>(status()) ||
            std::holds_alternative<PublicationAbandoned>(status());
    }
    EditorResult<void> ArtifactPublicationOperation::retry() { return impl_->retry(); }
    void ArtifactPublicationOperation::abandon() { impl_->abandon(); }
}
