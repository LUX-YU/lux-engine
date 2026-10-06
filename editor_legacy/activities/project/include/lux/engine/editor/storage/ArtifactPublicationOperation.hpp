#pragma once

#include <lux/engine/editor/storage/ProjectPublicationOperation.hpp>

namespace lux::editor::persistence { class DerivedArtifact; }
namespace lux::editor::sessions { class SessionStore; }

namespace lux::editor
{
    // Owns the frozen compile input and its package preparation. All disk and catalog steps
    // are delegated to ProjectPublicationOperation using the application's original write lane.
    class ArtifactPublicationOperation final
    {
    public:
        using CreateResult = EditorResult<std::unique_ptr<ArtifactPublicationOperation>>;
        [[nodiscard]] static CreateResult create(
            persistence::DerivedArtifact&, sessions::SessionStore&, ProjectStorage&,
            process::ExecutionRuntime&, persistence::WriteCoordinator&, persistence::IArtifactStore&,
            persistence::SaveExecution&
        );
        ~ArtifactPublicationOperation();
        ArtifactPublicationOperation(const ArtifactPublicationOperation&) = delete;
        ArtifactPublicationOperation& operator=(const ArtifactPublicationOperation&) = delete;
        ArtifactPublicationOperation(ArtifactPublicationOperation&&) = delete;
        ArtifactPublicationOperation& operator=(ArtifactPublicationOperation&&) = delete;
        void update();
        [[nodiscard]] const VPublicationStatus& status() const noexcept;
        [[nodiscard]] std::optional<persistence::WriteTicket> ticket() const noexcept;
        [[nodiscard]] std::string_view path() const noexcept;
        [[nodiscard]] bool terminal() const noexcept;
        [[nodiscard]] EditorResult<void> retry();
        void abandon();

    private:
        struct Impl;
        explicit ArtifactPublicationOperation(std::unique_ptr<Impl>) noexcept;
        std::unique_ptr<Impl> impl_;
    };
}
