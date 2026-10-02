#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <algorithm>
#include <random>

namespace lux::editor::application
{
    void EditorApplication::Impl::receiveArtifact(persistence::DerivedArtifact source)
    {
        const bool invalid = !source.valid();
        if (invalid)
        {
            result_failure_ = EditorFailure{EEditorError::INVALID_ARGUMENT, "artifact.input"};
            return;
        }
        if (phase_ != EApplicationPhase::RUNNING || artifacts_.size() == 64 || next_artifact_ == UINT64_MAX)
        {
            result_failure_ = EditorFailure{
                EEditorError::BUSY,
                "artifact.admission",
                0,
                "Close is pending or publication result capacity is full."
            };
            return;
        }
        // Called by a DIRECT intent during UI maintenance: retain the immutable capture only.
        artifacts_.push_back({next_artifact_++, std::move(source)});
    }
    EditorResult<void> EditorApplication::Impl::settleArtifacts()
    {
        using namespace persistence;
        EditorResult<void> outcome;
        for (auto& entry : artifacts_)
        {
            if (entry.settled)
                continue;
            if (!entry.ticket)
            {
                if (phase_ != EApplicationPhase::RUNNING)
                {
                    entry.failure = EditorFailure{EEditorError::CLOSING, "artifact.admission"};
                    entry.settled = true;
                    continue;
                }
                const auto source = entry.source.info().content;
                auto current = sessions_.describe(source.session);
                if (!current)
                {
                    if (current.error() == sessions::ESessionError::BUSY)
                        continue;
                    entry.failure = applicationFailure("artifact.source", current.error()).value();
                }
                else if (current->current != source)
                    entry.failure =
                        applicationFailure("artifact.source", sessions::ESessionError::STALE_CONTENT).value();
                else if (!current->binding)
                    entry.failure = applicationFailure("artifact.binding", EPersistenceError::UNBOUND).value();
                else
                {
                    const auto identity = entry.source.info().source_asset;
                    const auto* asset = project_->asset(current->binding->asset);
                    if (!asset || identity != asset->id)
                        entry.failure = applicationFailure("artifact.binding", EPersistenceError::STALE_SOURCE).value();
                    else
                        entry.asset = *asset;
                }
                if (entry.failure)
                {
                    entry.settled = true;
                    continue;
                }
                // Each accepted product gets an immutable physical version, also when its source later closes.
                std::mt19937 random{std::random_device{}()};
                entry.asset.cooked_path = ".lux/compiled/" + uuids::to_string(entry.asset.id.uuid()) + "/" +
                                          uuids::to_string(uuids::uuid_random_generator{random}()) + ".pak";
                auto target = files_.resolve(entry.asset.cooked_path);
                if (!target)
                {
                    entry.failure = applicationFailure("artifact.target", target.error()).value();
                    entry.settled = true;
                    continue;
                }
                if (target->expected_version != "missing")
                {
                    entry.failure = applicationFailure("artifact.immutable", EPersistenceError::CONFLICT).value();
                    entry.settled = true;
                    continue;
                }
                auto ticket = writes_.reserve(std::move(*target), {});
                if (!ticket)
                {
                    if (ticket.error().code == EPersistenceError::BUSY)
                        continue;
                    entry.failure = applicationFailure("artifact.reserve", ticket.error()).value();
                    entry.settled = true;
                    continue;
                }
                entry.ticket = *ticket;
                entry.encoding = true;
                auto submitted = project_tasks_.submit(
                    {"Package compiled asset", "Compiler"},
                    [cpu = engine_->execution().cpu(),
                     source = entry.source](process::TaskReporter reporter) mutable noexcept {
                        return stdexec::then(
                            stdexec::schedule(cpu),
                            [source = std::move(source), reporter]() -> PersistenceResult<CompiledPackage> {
                                if (reporter.stopToken().stop_requested())
                                    return cxx::unexpected(PersistenceFailure{EPersistenceError::CANCELLED});
                                auto encoded_source = source.encodeSource(reporter.stopToken());
                                if (!encoded_source)
                                    return cxx::unexpected(std::move(encoded_source.error()));
                                const auto digest = projectContentDigest(encoded_source->bytes.view());
                                const auto& info = source.info();
                                auto bytes = asset::encodePak(
                                    {{info.source_asset, info.primary_magic,
                                      uuids::to_string(info.source_asset.uuid()), {}, source.bytes()}},
                                    256U * 1024U * 1024U, "/Project"
                                );
                                if (!bytes)
                                    return cxx::unexpected(PersistenceFailure{
                                        EPersistenceError::ENCODE, bytes.error()
                                    });
                                auto frozen = EncodedArtifact{std::move(*bytes)};
                                return CompiledPackage{std::move(frozen.bytes), digest};
                            }
                        );
                    },
                    [this, id = entry.id](process::TTaskResult<CompiledPackage, PersistenceFailure>&& result) noexcept {
                        auto found = std::ranges::find(artifacts_, id, &ArtifactPresentation::id);
                        if (found == artifacts_.end())
                            std::terminate();
                        found->encoding = false;
                        if (result)
                            found->encoded.emplace(std::move(*result));
                        else if (auto* error = result.error().domainFailure())
                            found->encoded.emplace(cxx::unexpected(std::move(*error)));
                        else
                            found->encoded.emplace(cxx::unexpected(PersistenceFailure{
                                result.error().isCancelled() ? EPersistenceError::CANCELLED
                                                             : EPersistenceError::EXECUTION
                            }));
                    }
                );
                if (!submitted)
                {
                    entry.encoding = false;
                    entry.encoded.emplace(cxx::unexpected(PersistenceFailure{EPersistenceError::EXECUTION}));
                }
            }
            if (entry.encoding)
                continue;
            auto status = writes_.status(*entry.ticket);
            if (!status)
            {
                if (outcome)
                    outcome = applicationFailure("artifact.status", status.error());
                continue;
            }
            if (status->stage == EWriteStage::RESERVED && entry.encoded)
            {
                auto ready = *entry.encoded
                                 ? writes_.provideEncoded(*entry.ticket, EncodedArtifact{(**entry.encoded).bytes})
                                 : writes_.cancelBeforePublish(*entry.ticket, entry.encoded->error());
                if (!ready && outcome)
                    outcome = applicationFailure("artifact.encoded", ready.error());
                continue;
            }
            if (status->stage != EWriteStage::TERMINAL)
                continue; // UNKNOWN retains the original lane and result owner until explicit reconciliation.
            entry.result = status->outcome;
            const auto* published = std::get_if<CommitReceipt>(&*entry.result);
            if (published && !entry.failure)
            {
                if (!entry.package && !entry.reading)
                {
                    auto blocking = engine_->execution().blocking();
                    if (!blocking)
                    {
                        entry.failure = applicationFailure("artifact.scheduler", blocking.error()).value();
                        continue;
                    }
                    entry.reading = true;
                    auto submitted = project_tasks_.submit(
                        {"Read published package directory", "Storage"},
                        [scheduler = *blocking,
                         root = project_->root(),
                         path = entry.asset.cooked_path](process::TaskReporter) mutable noexcept {
                            return stdexec::then(
                                stdexec::schedule(scheduler),
                                [root = std::move(root), path = std::move(path)] {
                                    return readProjectPackage(root, path);
                                }
                            );
                        },
                        [this, id = entry.id](process::TTaskResult<ProjectPackage, EditorFailure>&& result) noexcept {
                            auto found = std::ranges::find(artifacts_, id, &ArtifactPresentation::id);
                            if (found == artifacts_.end())
                                std::terminate();
                            found->reading = false;
                            if (result)
                                found->package.emplace(std::move(*result));
                            else if (auto* error = result.error().domainFailure())
                                found->package.emplace(cxx::unexpected(std::move(*error)));
                            else
                                found->package.emplace(cxx::unexpected(
                                    EditorFailure{EEditorError::EXECUTION_FAILURE, "artifact.directory"}
                                ));
                        }
                    );
                    if (!submitted)
                    {
                        entry.reading = false;
                        entry.failure = applicationFailure("artifact.directory.submit", submitted.error()).value();
                    }
                    continue;
                }
                if (entry.reading)
                    continue;
                if (!*entry.package)
                    entry.failure = entry.package->error();
                if (!entry.failure && !entry.catalog)
                {
                    // Preserve a source save that completed after this compile capture. Publishing never
                    // changes History/checkpoint and never relabels an older compile as current source.
                    auto* current = project_->asset(entry.asset.id);
                    if (!current)
                    {
                        entry.failure = EditorFailure{EEditorError::STALE_REQUEST, "artifact.catalog.source"};
                        continue;
                    }
                    auto asset = *current;
                    asset.cooked_path = entry.asset.cooked_path;
                    asset.compiled_source_digest = (**entry.encoded).source_digest;
                    ProjectUpdate update;
                    update.assets.push_back(std::move(asset));
                    auto candidate = project_->preparePublication(update);
                    if (!candidate)
                    {
                        if (candidate.error().code == EEditorError::BUSY)
                            continue;
                        entry.failure = candidate.error();
                    }
                    else
                    {
                        auto encoded = encodeProjectManifest(candidate->manifest);
                        auto target = files_.resolve(candidate->manifest_path);
                        if (!encoded)
                            entry.failure = applicationFailure("artifact.catalog.encode", encoded.error()).value();
                        else if (!target)
                            entry.failure = applicationFailure("artifact.catalog.target", target.error()).value();
                        else
                        {
                            target->expected_version = candidate->before_manifest_digest;
                            const auto bytes = std::as_bytes(std::span(*encoded));
                            auto ticket = publishEncodedArtifact(
                                writes_,
                                std::move(*target),
                                EncodedArtifact{{bytes.begin(), bytes.end()}}
                            );
                            if (!ticket)
                                entry.failure = applicationFailure("artifact.catalog.publish", ticket.error()).value();
                            else
                            {
                                entry.catalog = std::move(*candidate);
                                entry.catalog_ticket = *ticket;
                            }
                        }
                    }
                }
                if (entry.catalog_ticket)
                {
                    auto written = writes_.status(*entry.catalog_ticket);
                    if (!written)
                    {
                        if (outcome)
                            outcome = applicationFailure("artifact.catalog.status", written.error());
                        continue;
                    }
                    if (written->stage != EWriteStage::TERMINAL)
                        continue;
                    if (const auto* receipt = std::get_if<CommitReceipt>(&*written->outcome))
                    {
                        ProjectPublicationReceipt adoption{
                            entry.catalog->manifest,
                            receipt->version,
                            1,
                            {},
                            {{entry.asset.cooked_path, published->version}},
                            {**entry.package}
                        };
                        auto adopted = project_->adoptPublication(*entry.catalog, adoption);
                        if (!adopted)
                        {
                            if (adopted.error().code == EEditorError::BUSY)
                                continue;
                            entry.failure = adopted.error();
                        }
                    }
                    else
                        entry.failure = applicationFailure("artifact.catalog.publication", *written->outcome).value();
                    auto acknowledged = writes_.acknowledge(*entry.catalog_ticket);
                    if (!acknowledged)
                    {
                        if (outcome)
                            outcome = applicationFailure("artifact.catalog.acknowledge", acknowledged.error());
                        continue;
                    }
                    entry.catalog_ticket.reset();
                    entry.catalog.reset();
                }
            }
            auto acknowledged = writes_.acknowledge(*entry.ticket);
            if (!acknowledged)
            {
                if (outcome)
                    outcome = applicationFailure("artifact.acknowledge", acknowledged.error());
                continue;
            }
            entry.settled = true;
        }
        return outcome;
    }
}
