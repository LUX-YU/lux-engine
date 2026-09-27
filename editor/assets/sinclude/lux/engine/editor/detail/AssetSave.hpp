#pragma once

#include <lux/engine/editor/AssetSave.hpp>
#include <lux/engine/editor/detail/ProjectWrite.hpp>
#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <lux/engine/editor/editing/EditHistory.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/resource/asset/Asset.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>

namespace lux::editor::detail
{
    // A fixed publication precondition, not an entry inserted into the live catalog.
    struct AssetSaveTarget final
    {
        ProjectAssetEntry entry;
        std::string before_digest;
    };

    [[nodiscard]] inline EditorResult<AssetSaveTarget> captureAssetSaveTarget(
        ProjectStorage& project,
        asset::AssetId id
    )
    {
        const auto* entry = project.asset(id);
        if (!entry)
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "asset.save.target"});
        return AssetSaveTarget{*entry, std::string(project.sourceDigest(entry->source_path))};
    }

    [[nodiscard]] inline EditorResult<AssetSaveTarget> newAssetSaveTarget(
        ProjectStorage& project,
        asset::AssetId id,
        EProjectAssetKind kind,
        std::string_view vpath
    )
    {
        constexpr std::string_view mount = "/Project/";
        if (!vpath.starts_with(mount) || id.isNull() || project.asset(id))
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "asset.save.new-target"});
        const auto relative = vpath.substr(mount.size());
        if (!validProjectPath(relative))
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "asset.save.path"});
        ProjectAssetEntry entry{id, kind, "Content/" + std::string(relative)};
        entry.mount_path = std::string(relative);
        auto manifest = project.manifest();
        manifest.assets.push_back(entry);
        const auto valid = validateProjectManifest(manifest);
        if (!valid)
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::INVALID_ARGUMENT,
                "asset.save.new-target",
                0,
                valid.error().field,
                valid.error()
            });
        return AssetSaveTarget{std::move(entry), "missing"};
    }

    template <class Capture> struct TCompiledCapture final
    {
        std::shared_ptr<const Capture> source;
        asset::PakWriteEntry artifact;
    };

    template <class Asset, class Capture> struct TCompiledAsset final
    {
        std::shared_ptr<const Asset> artifact;
        TCompiledCapture<Capture> publication;
    };

    // Compile encodes the GPU input once. Source/pak encoding happens only when publishing this capture.
    template <class Asset, class Capture>
    EditorResult<TCompiledAsset<Asset, Capture>> encodeCompiledAsset(
        std::shared_ptr<const Asset> artifact,
        Capture source,
        std::string path
    )
    {
        constexpr std::size_t byte_limit = 256U * 1024U * 1024U;
        auto encoded = asset::TAssetSerDeser<Asset>::encode(*artifact, asset::AssetEncodeLimits{byte_limit});
        if (!encoded)
        {
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::SOURCE_FAILURE,
                "asset.artifact.encode",
                static_cast<std::uint64_t>(encoded.error().code),
                {},
                encoded.error()
            });
        }
        auto bytes = std::make_shared<const std::vector<std::byte>>(std::move(*encoded));
        asset::PakWriteEntry entry{
            artifact->id(),
            Asset::primary_magic,
            std::move(path),
            {},
            lux::cxx::SharedBytes<>::fromOwner(bytes, std::span(*bytes))
        };
        return TCompiledAsset<Asset, Capture>{
            std::move(artifact),
            {std::make_shared<const Capture>(std::move(source)), std::move(entry)}
        };
    }

    // An admitted save owns one capture and one history ticket through all retries.
    template <class Capture, class Encoder> class TAssetSave final
    {
        using VCaptureStorage = std::variant<Capture, TCompiledCapture<Capture>>;
        struct Encoded final
        {
            lux::cxx::SharedBytes<> bytes;
            std::string digest;
            lux::cxx::SharedBytes<> package;
            std::string package_digest;
        };
        struct Encode final
        {
            const VCaptureStorage* capture;
            std::stop_token stop;
            EditorResult<Encoded> operator()() const noexcept
            {
                auto encoded = std::visit(
                    [&](const auto& value) -> EditorResult<lux::cxx::SharedBytes<>> {
                        if constexpr (std::is_same_v<std::decay_t<decltype(value)>, Capture>)
                        {
                            return Encoder{}(value, stop);
                        }
                        else
                        {
                            return Encoder{}(*value.source, stop);
                        }
                    },
                    *capture
                );
                if (!encoded)
                {
                    return lux::cxx::unexpected(std::move(encoded.error()));
                }
                const auto digest = projectContentDigest(encoded->view());
                Encoded result{std::move(*encoded), digest};
                if (const auto* compiled = std::get_if<TCompiledCapture<Capture>>(capture))
                {
                    auto package = asset::encodePak({compiled->artifact}, 256U * 1024U * 1024U, "/Project");
                    if (!package)
                        return lux::cxx::unexpected(
                            EditorFailure{EEditorError::SOURCE_FAILURE, "asset.publish.package", 0, package.error()}
                        );
                    auto owner = std::make_shared<const std::vector<std::byte>>(std::move(*package));
                    result.package = lux::cxx::SharedBytes<>::fromOwner(owner, std::span(*owner));
                    result.package_digest = projectContentDigest(result.package.view());
                }
                return result;
            }
        };
        struct Encoding final
        {};
        struct Idle final
        {};

    public:
        template <class SavedCapture>
        TAssetSave(
            SaveRequestId id,
            editing::SaveTicket ticket,
            editing::Revision revision,
            AssetSaveTarget target,
            SavedCapture capture,
            ProjectStorage& project,
            process::ExecutionRuntime& runtime,
            editing::EditHistory& history,
            process::CompletionWork::Request completed = {}
        )
            : id_(id), ticket_(ticket), revision_(revision), capture_(std::move(capture)), project_(project),
              runtime_(runtime), history_(&history), source_(std::move(target.entry)),
              before_digest_(std::move(target.before_digest)), completed_(std::move(completed)),
              adoption_(
                  runtime,
                  this,
                  [](void* owner) noexcept {
                      auto& save = *static_cast<TAssetSave*>(owner);
                      save.adoptCompleted();
                      save.completed_.request();
                  }
              ),
              tasks_(runtime)
        {
            startEncoding();
        }
        ~TAssetSave()
        {
            if (!runtime_.waitUntil([this]() noexcept { return !std::holds_alternative<SavePending>(status_); }))
                std::terminate();
            adoption_.cancel();
            if (history_)
            {
                // Failed saves remain dirty; destroying the presenter must not strand its history ticket.
                const auto finished = history_->finishSave(ticket_, editing::ESaveOutcome::FAILED);
                if (!finished)
                    log::error("asset.save", "Failed to release save ticket ({})", unsigned(finished.error().code));
                if (const auto* failure = std::get_if<SaveRetryable>(&status_))
                    log::error("asset.save", "{}: {}", failure->failure.domain, failure->failure.message);
            }
        }
        TAssetSave(const TAssetSave&) = delete;
        TAssetSave(TAssetSave&&) = delete;

        SaveRequestId id() const noexcept
        {
            return id_;
        }
        std::span<const SaveRequestId> requests() const noexcept
        {
            return {&id_, 1};
        }
        const VSaveRequestStatus& status() const noexcept
        {
            return status_;
        }
        bool terminal() const noexcept
        {
            return std::holds_alternative<SaveSucceeded>(status_) || std::holds_alternative<SaveAbandoned>(status_);
        }
        EditorResult<void> retry(bool allow_new_work = true)
        {
            if (!allow_new_work && !abandoning_)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::CLOSING, "asset.save.retry"});
            }
            if (!std::holds_alternative<SaveRetryable>(status_) || finishing_)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "asset.save.retry"});
            }
            if (const auto& failure = std::get<SaveRetryable>(status_); !failure.retry_allowed)
            {
                return lux::cxx::unexpected(failure.failure);
            }
            ++attempt_;
            adoption_.request();
            if (auto* write = std::get_if<ProjectWrite>(&work_))
            {
                if (write->terminal())
                {
                    finishTicket();
                    return {};
                }
                auto result = write->retry();
                if (result)
                {
                    status_ = SavePending{abandoning_ ? ESaveStage::ABANDONING : ESaveStage::PUBLISHING, attempt_};
                }
                return result;
            }
            if (abandoning_)
            {
                finishTicket();
            }
            else if (image_.bytes.empty())
            {
                startEncoding();
            }
            else
            {
                status_ = SavePending{ESaveStage::WAITING_FOR_PROJECT, attempt_};
            }
            return {};
        }
        void abandon()
        {
            if (terminal() || abandoning_)
            {
                return;
            }
            abandoning_ = true;
            adoption_.request();
            stop_.request_stop();
            if (auto* write = std::get_if<ProjectWrite>(&work_))
            {
                write->abandon();
            }
            else if (work_.index() == 0 && !finishing_)
            {
                finishTicket();
            }
        }

    private:
        void adoptCompleted()
        {
            if (finishing_)
            {
                return;
            }
            if (encoded_)
            {
                auto result = std::move(*encoded_);
                encoded_.reset();
                work_.template emplace<Idle>();
                if (abandoning_)
                {
                    finishTicket();
                    return;
                }
                if (!result)
                {
                    const bool retry = result.error().code == EEditorError::EXECUTION_FAILURE ||
                                       result.error().code == EEditorError::BUSY;
                    failed(std::move(result.error()), retry);
                    return;
                }
                image_ = std::move(*result);
                status_ = SavePending{ESaveStage::WAITING_FOR_PROJECT, attempt_};
            }
            if (auto* write = std::get_if<ProjectWrite>(&work_))
            {
                if (const auto* error = std::get_if<EditorFailure>(&write->status()))
                {
                    failed(*error);
                }
                else if (write->terminal())
                {
                    finishTicket();
                }
                else
                {
                    status_ = SavePending{abandoning_ ? ESaveStage::ABANDONING : ESaveStage::PUBLISHING, attempt_};
                }
                return;
            }
            const auto* pending = std::get_if<SavePending>(&status_);
            if (!pending || pending->stage != ESaveStage::WAITING_FOR_PROJECT || abandoning_)
            {
                return;
            }

            // Capture source facts; adopt the latest artifact reference while waiting for the Project slot.
            const auto* current = project_.asset(source_.id);
            const bool creating = before_digest_ == "missing";
            const bool invalid_creation = creating && current;
            const bool invalid_existing =
                !creating && (!current || current->source_path != source_.source_path || current->kind != source_.kind);
            if (invalid_creation || invalid_existing)
            {
                failed(EditorFailure{EEditorError::STALE_REQUEST, "asset.save.source"});
                return;
            }
            auto entry = creating ? source_ : *current;
            entry.source_digest = image_.digest;
            ProjectUpdate update{{}, {}, {{source_.source_path, before_digest_, image_.bytes}}};
            if (!image_.package.empty())
            {
                entry.compiled_source_digest = image_.digest;
                entry.cooked_path =
                    ".lux/compiled/" + uuids::to_string(source_.id.uuid()) + "/" + image_.package_digest + ".luxpak";
                if (project_.sourceDigest(entry.cooked_path) != image_.package_digest)
                {
                    update.files.push_back({entry.cooked_path, "missing", image_.package});
                }
            }
            update.assets.push_back(std::move(entry));
            auto prepared = project_.preparePublication(update);
            if (!prepared)
            {
                if (prepared.error().code != EEditorError::BUSY)
                {
                    failed(std::move(prepared.error()));
                }
                else
                    project_.whenPublicationAvailable(adoption_.requester());
                return;
            }
            status_ = SavePending{ESaveStage::PUBLISHING, attempt_};
            work_.template emplace<ProjectWrite>(project_, runtime_, std::move(*prepared), adoption_.requester());
        }

    private:
        void startEncoding()
        {
            status_ = SavePending{ESaveStage::ENCODING, attempt_};
            work_.template emplace<Encoding>();
            auto started = tasks_.submit(
                {"Encode asset", "Storage"},
                [this](process::TaskReporter) noexcept {
                    return stdexec::then(stdexec::schedule(runtime_.cpu()), Encode{&capture_, stop_.get_token()});
                },
                [this](process::TTaskResult<Encoded, EditorFailure>&& result) noexcept {
                    encoded_.emplace(taskResult(std::move(result)));
                    adoption_.request();
                }
            );
            if (!started)
            {
                work_.template emplace<Idle>();
                failed(EditorFailure{EEditorError::EXECUTION_FAILURE, "asset.save.submit", 0, {}, started.error()});
                completed_.request();
            }
        }
        void failed(EditorFailure error, bool retry = true)
        {
            retry &= error.code != EEditorError::READ_ONLY && error.code != EEditorError::MISSING_PROVIDER &&
                     error.code != EEditorError::INVALID_ARGUMENT && error.code != EEditorError::STALE_REQUEST &&
                     error.code != EEditorError::STALE_DOCUMENT;
            if (const auto* publication = std::any_cast<ProjectPublicationFailure>(&error.cause))
            {
                retry &= publication->code != EProjectPublicationError::INVALID_PATH &&
                         publication->code != EProjectPublicationError::CONFLICT &&
                         (publication->code != EProjectPublicationError::RECOVERY_CONFLICT || abandoning_);
            }
            status_ = SaveRetryable{std::move(error), ticket_.state(), attempt_, retry};
        }
        void finishTicket()
        {
            if (finishing_)
            {
                return;
            }
            finishing_ = true;
            const auto* write = std::get_if<ProjectWrite>(&work_);
            const auto* published = write ? std::get_if<PublicationSucceeded>(&write->status()) : nullptr;
            const auto outcome = published ? editing::ESaveOutcome::SUCCEEDED : editing::ESaveOutcome::CANCELLED;
            const auto finished = history_->finishSave(ticket_, outcome);
            if (!finished)
            {
                failed(EditorFailure{
                    EEditorError::INVALID_STATE,
                    "asset.save.ticket",
                    static_cast<std::uint64_t>(finished.error().code),
                    {},
                    finished.error()
                });
                finishing_ = false;
                return;
            }
            if (published)
            {
                status_ = SaveSucceeded{ticket_.state(), revision_, published->cleanup};
            }
            else
            {
                status_ = SaveAbandoned{};
            }
            work_.template emplace<Idle>();
            history_ = nullptr; // Terminal requests may outlive a tool's previous HistoryId.
            finishing_ = false;
        }
        SaveRequestId id_;
        editing::SaveTicket ticket_;
        editing::Revision revision_;
        VCaptureStorage capture_;
        ProjectStorage& project_;
        process::ExecutionRuntime& runtime_;
        editing::EditHistory* history_;
        ProjectAssetEntry source_;
        std::string before_digest_;
        Encoded image_;
        std::stop_source stop_;
        std::uint64_t attempt_{1};
        bool abandoning_{};
        bool finishing_{};
        VSaveRequestStatus status_;
        process::CompletionWork::Request completed_;
        process::CompletionWork adoption_;
        std::optional<EditorResult<Encoded>> encoded_;
        std::variant<Idle, Encoding, ProjectWrite> work_;
        process::TaskScope tasks_;
    };
} // namespace lux::editor::detail
