#pragma once

#include <lux/engine/editor/detail/ProjectWrite.hpp>
#include <lux/engine/editor/editing/EditHistory.hpp>
#include <lux/engine/editor/project/Project.hpp>
#include <lux/engine/resource/asset/Asset.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>

namespace lux::editor::detail
{
    struct CompiledDocumentImage final
    {
        lux::cxx::SharedBytes<> source;
        lux::cxx::SharedBytes<> package;
        std::string package_digest;
    };

    template <class Asset> struct CompiledDocument final
    {
        std::shared_ptr<const Asset> artifact;
        CompiledDocumentImage image;
    };

    // Called on CPU after compilation. Package bytes and source bytes describe the same fixed capture.
    template <class Asset>
    EditorResult<CompiledDocument<Asset>> encodeCompiledDocument(std::shared_ptr<const Asset> artifact,
                                                                 lux::cxx::SharedBytes<> source, std::string path)
    {
        constexpr std::size_t byte_limit = 256U * 1024U * 1024U;
        auto encoded = asset::TAssetSerDeser<Asset>::encode(*artifact, asset::AssetEncodeLimits{byte_limit});
        if (!encoded)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE,
                                                      "document.artifact.encode",
                                                      static_cast<std::uint64_t>(encoded.error().code),
                                                      {},
                                                      encoded.error()});
        }
        auto bytes = std::make_shared<const std::vector<std::byte>>(std::move(*encoded));
        std::vector<asset::PakWriteEntry> entries{{artifact->id(),
                                                   Asset::primary_magic,
                                                   std::move(path),
                                                   {},
                                                   lux::cxx::SharedBytes<>::fromOwner(bytes, std::span(*bytes))}};
        auto pak = asset::encodePak(entries, byte_limit, "/Project");
        if (!pak)
        {
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::SOURCE_FAILURE, "document.artifact.pak", 0, pak.error()});
        }
        auto image = std::make_shared<const std::vector<std::byte>>(std::move(*pak));
        auto package = lux::cxx::SharedBytes<>::fromOwner(image, std::span(*image));
        auto digest = projectContentDigest(package.view());
        return CompiledDocument<Asset>{std::move(artifact), {std::move(source), std::move(package), std::move(digest)}};
    }

    // An admitted save owns one capture and one history ticket through all retries.
    template <class Capture, class Encoder> class DocumentSave final
    {
        using CaptureStorage = std::variant<Capture, CompiledDocumentImage>;
        struct Encoded final
        {
            lux::cxx::SharedBytes<> bytes;
            std::string digest;
        };
        struct Encode final
        {
            const CaptureStorage *capture;
            std::stop_token stop;
            EditorResult<Encoded> operator()() const noexcept
            {
                auto encoded = std::visit(
                    [&](const auto &value) -> EditorResult<lux::cxx::SharedBytes<>>
                    {
                        if constexpr (std::is_same_v<std::decay_t<decltype(value)>, Capture>)
                        {
                            return Encoder{}(value, stop);
                        }
                        else
                        {
                            return value.source;
                        }
                    },
                    *capture);
                if (!encoded)
                {
                    return lux::cxx::unexpected(std::move(encoded.error()));
                }
                const auto digest = projectContentDigest(encoded->view());
                return Encoded{std::move(*encoded), digest};
            }
        };
        using Encoding = ScheduledDocumentTask<process::CpuScheduler, Encode>;
        struct Idle final
        {
        };

      public:
        template <class SavedCapture>
        DocumentSave(SaveRequestId id, editing::SaveTicket ticket, editing::Revision revision,
                     lux::asset::AssetId source, SavedCapture capture, Project &project,
                     process::ExecutionRuntime &runtime, editing::EditHistory &history)
            : id_(id), ticket_(ticket), revision_(revision), capture_(std::move(capture)), project_(project),
              runtime_(runtime), history_(history), source_(*project.asset(source)),
              before_digest_(project.sourceDigest(source_.source_path))
        {
            startEncoding();
        }
        ~DocumentSave()
        {
            if (!terminal())
            {
                std::terminate();
            }
        }
        DocumentSave(const DocumentSave &) = delete;
        DocumentSave(DocumentSave &&) = delete;

        SaveRequestId id() const noexcept
        {
            return id_;
        }
        std::span<const SaveRequestId> requests() const noexcept
        {
            return {&id_, 1};
        }
        const SaveRequestStatus &status() const noexcept
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
                return lux::cxx::unexpected(EditorFailure{EEditorError::CLOSING, "document.save.retry"});
            }
            if (!std::holds_alternative<SaveRetryable>(status_) || finishing_)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "document.save.retry"});
            }
            if (const auto &failure = std::get<SaveRetryable>(status_); !failure.retry_allowed)
            {
                return lux::cxx::unexpected(failure.failure);
            }
            ++attempt_;
            if (auto *write = std::get_if<ProjectWrite>(&work_))
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
            stop_.request_stop();
            if (auto *write = std::get_if<ProjectWrite>(&work_))
            {
                write->abandon();
            }
            else if (work_.index() == 0 && !finishing_)
            {
                finishTicket();
            }
        }
        void poll()
        {
            if (finishing_)
            {
                return;
            }
            if (auto *work = std::get_if<Encoding>(&work_); work && work->ready())
            {
                auto result = work->take();
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
            if (auto *write = std::get_if<ProjectWrite>(&work_))
            {
                write->poll();
                if (const auto *error = std::get_if<EditorFailure>(&write->status()))
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
            const auto *pending = std::get_if<SavePending>(&status_);
            if (!pending || pending->stage != ESaveStage::WAITING_FOR_PROJECT || abandoning_)
            {
                return;
            }

            // Capture source facts; adopt the latest artifact reference while waiting for the Project slot.
            const auto *current = project_.asset(source_.id);
            if (!current || current->source_path != source_.source_path)
            {
                failed(EditorFailure{EEditorError::STALE_REQUEST, "document.save.source"});
                return;
            }
            auto entry = *current;
            entry.source_digest = image_.digest;
            ProjectUpdate update{{}, {}, {{source_.source_path, before_digest_, image_.bytes}}};
            if (const auto *compiled = std::get_if<CompiledDocumentImage>(&capture_))
            {
                entry.compiled_source_digest = image_.digest;
                entry.cooked_path =
                    ".lux/compiled/" + uuids::to_string(source_.id.uuid()) + "/" + compiled->package_digest + ".luxpak";
                if (project_.sourceDigest(entry.cooked_path) != compiled->package_digest)
                {
                    update.files.push_back({entry.cooked_path, "missing", compiled->package});
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
                return;
            }
            status_ = SavePending{ESaveStage::PUBLISHING, attempt_};
            work_.template emplace<ProjectWrite>(project_, runtime_, std::move(*prepared));
        }

      private:
        void startEncoding()
        {
            status_ = SavePending{ESaveStage::ENCODING, attempt_};
            work_
                .template emplace<Encoding>(
                    runtime_, stdexec::then(stdexec::schedule(runtime_.cpu()), Encode{&capture_, stop_.get_token()}))
                .start();
        }
        void failed(EditorFailure error, bool retry = true)
        {
            retry &= error.code != EEditorError::READ_ONLY && error.code != EEditorError::MISSING_PROVIDER &&
                     error.code != EEditorError::INVALID_ARGUMENT && error.code != EEditorError::STALE_REQUEST &&
                     error.code != EEditorError::STALE_DOCUMENT;
            if (const auto *publication = std::any_cast<ProjectPublicationFailure>(&error.cause))
            {
                retry &= publication->code != EProjectPublicationError::INVALID_PATH &&
                         publication->code != EProjectPublicationError::CONFLICT &&
                         publication->code != EProjectPublicationError::RECOVERY_CONFLICT;
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
            const auto *write = std::get_if<ProjectWrite>(&work_);
            const auto *published = write ? std::get_if<PublicationSucceeded>(&write->status()) : nullptr;
            const auto outcome = published ? editing::ESaveOutcome::SUCCEEDED : editing::ESaveOutcome::CANCELLED;
            const auto finished = history_.finishSave(ticket_, outcome);
            if (!finished)
            {
                failed(EditorFailure{EEditorError::INVALID_STATE,
                                     "document.save.ticket",
                                     static_cast<std::uint64_t>(finished.error().code),
                                     {},
                                     finished.error()});
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
            finishing_ = false;
        }
        SaveRequestId id_;
        editing::SaveTicket ticket_;
        editing::Revision revision_;
        CaptureStorage capture_;
        Project &project_;
        process::ExecutionRuntime &runtime_;
        editing::EditHistory &history_;
        ProjectAssetEntry source_;
        std::string before_digest_;
        Encoded image_;
        std::stop_source stop_;
        std::uint64_t attempt_{1};
        bool abandoning_{};
        bool finishing_{};
        SaveRequestStatus status_;
        std::variant<Idle, Encoding, ProjectWrite> work_;
    };
} // namespace lux::editor::detail
