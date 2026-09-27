#pragma once

#include <lux/engine/editor/detail/TaskResult.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>

namespace lux::editor::detail
{
    // Snapshot acquisition stays on the owner. IO and decoding receive only fixed values.
    template <class Codec>
    [[nodiscard]] auto readAssetSource(
        ProjectStorage& project,
        const ProjectAssetEntry& entry,
        process::ExecutionRuntime& execution,
        process::TaskReporter reporter,
        Codec codec = {}
    )
    {
        auto read = stdexec::then(
            stdexec::schedule(*execution.blocking()),
            [view = project.captureSource(entry.id, Codec::max_bytes),
             id = entry.id,
             digest = std::string(project.sourceDigest(entry.source_path)),
             reporter]() noexcept -> EditorResult<lux::cxx::SharedBytes<>> {
                reporter.setPhase("Read source");
                if (reporter.stopToken().stop_requested())
                    return lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "asset.read"});
                if (!view)
                    return lux::cxx::unexpected(view.error());
                auto bytes = view->open(id);
                if (!bytes)
                    return lux::cxx::unexpected(EditorFailure{
                        EEditorError::SOURCE_FAILURE,
                        "asset.read",
                        static_cast<std::uint64_t>(bytes.error()),
                        {},
                        bytes.error()
                    });
                if (projectContentDigest(bytes->bytes.view()) != digest)
                    return lux::cxx::unexpected(EditorFailure{
                        EEditorError::SOURCE_FAILURE,
                        "asset.source.conflict",
                        static_cast<std::uint64_t>(EProjectPublicationError::CONFLICT)
                    });
                return std::move(bytes->bytes);
            }
        );
        return stdexec::then(
            stdexec::continues_on(std::move(read), execution.cpu()),
            [codec = std::move(codec), id = entry.id, reporter](EditorResult<lux::cxx::SharedBytes<>> bytes
            ) noexcept -> EditorResult<typename Codec::Source> {
                if (!bytes)
                    return lux::cxx::unexpected(std::move(bytes.error()));
                reporter.setPhase("Decode source");
                if (reporter.stopToken().stop_requested())
                    return lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "asset.decode"});
                auto source = codec.decode(*bytes, reporter.stopToken());
                if (source && Codec::identity(*source) != id)
                    return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "asset.identity"});
                return source;
            }
        );
    }
}
