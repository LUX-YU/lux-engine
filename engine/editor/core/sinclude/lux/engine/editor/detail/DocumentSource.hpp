#pragma once

#include <fstream>
#include <lux/engine/editor/DocumentEditor.hpp>
#include <lux/engine/editor/detail/DocumentTask.hpp>
#include <lux/engine/editor/project/Project.hpp>

namespace lux::editor::detail
{
// Read the project source on Blocking, decode on CPU, retain one terminal for the owner.
// Codec supplies source semantics; it neither discovers a scheduler nor creates a GUI.
template <class Codec> class SourceOpening final : public DocumentOpening
{
    using Source = typename Codec::Source;
    using ReadResult = EditorResult<lux::cxx::SharedBytes<>>;
    using Result = EditorResult<Source>;
    static constexpr bool HasPreparation =
        requires(Codec &codec, Source &source, Project &project, process::ExecutionRuntime &runtime,
                 std::stop_token stop) { codec.prepare(source, project, runtime, stop); };
    struct Read final
    {
        std::filesystem::path path;
        std::string expected_digest;
        std::stop_token stop;

        ReadResult operator()() const noexcept
        {
            if (stop.stop_requested())
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "document.read"});
            }
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            if (error || size > Codec::max_bytes)
            {
                return lux::cxx::unexpected(EditorFailure{error ? EEditorError::SOURCE_FAILURE : EEditorError::CAPACITY,
                                                          "document.read", static_cast<std::uint64_t>(error.value()),
                                                          path.string(), error});
            }
            auto owner = std::make_shared<std::vector<std::byte>>(static_cast<std::size_t>(size));
            std::ifstream file(path, std::ios::binary);
            if (!file.read(reinterpret_cast<char *>(owner->data()), static_cast<std::streamsize>(size)))
            {
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::SOURCE_FAILURE, "document.read", 0, path.string()});
            }
            if (file.peek() != std::char_traits<char>::eof() || file.bad() ||
                projectContentDigest(*owner) != expected_digest)
            {
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::SOURCE_FAILURE, "document.source.conflict",
                                  static_cast<std::uint64_t>(EProjectPublicationError::CONFLICT), path.string()});
            }
            return lux::cxx::SharedBytes<>::fromOwner(owner, *owner);
        }
    };
    struct Decode final
    {
        const Codec *codec;
        lux::asset::AssetId identity;
        std::stop_token stop;
        Result operator()(ReadResult bytes) const noexcept
        {
            if (!bytes)
            {
                return lux::cxx::unexpected(std::move(bytes.error()));
            }
            if (stop.stop_requested())
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "document.decode"});
            }
            auto result = codec->decode(*bytes, stop);
            if (result && codec->identity(*result) != identity)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "document.identity"});
            }
            return result;
        }
    };
    using ReadSender =
        decltype(stdexec::then(stdexec::schedule(std::declval<process::BlockingScheduler>()), std::declval<Read>()));
    using Sender =
        decltype(stdexec::then(stdexec::continues_on(std::declval<ReadSender>(), std::declval<process::CpuScheduler>()),
                               std::declval<Decode>()));

  public:
    SourceOpening(Project &project, const ProjectAssetEntry &entry, process::ExecutionRuntime &runtime,
                  Codec codec = {})
        : codec_(std::move(codec)), project_(project), runtime_(runtime),
          task_(runtime, stdexec::then(stdexec::continues_on(
                                           stdexec::then(stdexec::schedule(*runtime.blocking()),
                                                         Read{project.root() / entry.source_path,
                                                              std::string(project.sourceDigest(entry.source_path)),
                                                              stop_.get_token()}),
                                           runtime.cpu()),
                                       Decode{&codec_, entry.id, stop_.get_token()}))
    {
        task_.start();
    }
    void cancel() noexcept override
    {
        stop_.request_stop();
        if constexpr (HasPreparation)
        {
            if (!taken_)
            {
                preparation_done_ = false;
            }
        }
    }
    void poll() override
    {
        if constexpr (HasPreparation)
        {
            if (preparation_done_ || (prepared_.index() == 0 && !task_.ready()))
            {
                return;
            }
            if (prepared_.index() == 0)
            {
                prepared_.template emplace<Result>(task_.take());
            }
            auto &source = std::get<Result>(prepared_);
            if (!source)
            {
                preparation_done_ = true;
                return;
            }
            auto ready = codec_.prepare(*source, project_, runtime_, stop_.get_token());
            if (!ready)
            {
                source = lux::cxx::unexpected(std::move(ready.error()));
                preparation_done_ = true;
            }
            else
            {
                preparation_done_ = *ready;
            }
        }
    }
    bool settled() const noexcept override
    {
        if constexpr (HasPreparation)
        {
            return preparation_done_;
        }
        return task_.ready();
    }
    EditorResult<std::unique_ptr<DocumentEditor>> take() override
    {
        if (!settled() || taken_)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "document.open.take"});
        }
        taken_ = true;
        auto result = [&]() -> Result
        {
            if constexpr (HasPreparation)
            {
                return std::move(std::get<Result>(prepared_));
            }
            return task_.take();
        }();
        if (!result)
        {
            return lux::cxx::unexpected(std::move(result.error()));
        }
        if (stop_.stop_requested())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "document.open"});
        }
        return codec_.adopt(*result, project_, runtime_);
    }

  private:
    Codec codec_;
    Project &project_;
    process::ExecutionRuntime &runtime_;
    std::stop_source stop_;
    bool taken_{};
    bool preparation_done_{};
    std::variant<std::monostate, Result> prepared_;
    DocumentTask<Result, Sender> task_;
};
} // namespace lux::editor::detail
