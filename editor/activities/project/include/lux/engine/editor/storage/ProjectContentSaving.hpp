#pragma once

#include <lux/engine/editor/persistence/SaveTypes.hpp>
#include <lux/engine/editor/persistence/DerivedArtifact.hpp>
#include <lux/engine/editor/storage/ProjectPublication.hpp>
#include <span>

namespace lux::services
{
    struct ServiceDescriptor;
}
namespace lux::editor::persistence
{
    class SaveService;
    class WriteCoordinator;
    class IArtifactStore;
    class SaveExecution;
    class DerivedArtifact;
} // namespace lux::editor::persistence
namespace lux::process
{
    class ExecutionRuntime;
}
namespace lux::editor::sessions
{
    class SessionStore;
    class SessionOpening;
    struct SaveAllEntry;
} // namespace lux::editor::sessions
namespace lux::editor
{
    extern const services::ServiceDescriptor kProjectContentSavingService;
    // Immutable observation of the existing publication owner, never another disk/result authority.
    struct ArtifactPublicationReport final
    {
        std::uint64_t id{};
        VPublicationStatus status;
        std::string path;
        std::optional<persistence::WriteTicket> ticket;
        bool admitted{}, terminal{};
    };
    struct PreparedProjectSave final
    {
        persistence::SaveRequest request;
        ProjectAssetEntry asset;
    };
    class ProjectSaveReport final
    {
    public:
        ProjectSaveReport(persistence::SaveId, ProjectAssetEntry);
        ProjectSaveReport(ProjectSaveReport&&) noexcept = default;
        ProjectSaveReport& operator=(ProjectSaveReport&&) noexcept = default;
        ProjectSaveReport(const ProjectSaveReport&) = delete;
        ProjectSaveReport& operator=(const ProjectSaveReport&) = delete;

        persistence::SaveId id;
        ProjectAssetEntry asset;
        std::optional<persistence::SaveOutcome> result;
        std::optional<persistence::WriteTicket> catalog_ticket;
        std::optional<EditorFailure> failure;

    private:
        friend class ProjectContentSaving;
        std::optional<PreparedProjectPublication> catalog_;
    };
    // Owns source-save/catalog association and compiled publication requests/results. Encoding, disk
    // publication and checkpoint adoption remain in the existing operations and WriteCoordinator.
    // Drive accepted work to settled() before destruction, while those borrowed owners are still alive.
    class ProjectContentSaving final : public persistence::IArtifactSubmission
    {
    public:
        ProjectContentSaving(
            sessions::SessionStore&,
            sessions::SessionOpening&,
            persistence::SaveService&,
            ProjectStorage&,
            persistence::WriteCoordinator&,
            persistence::IArtifactStore&,
            process::ExecutionRuntime&,
            persistence::SaveExecution&
        );
        // All shared providers must be non-null; final release follows operation/result cleanup.
        ProjectContentSaving(
            std::shared_ptr<sessions::SessionStore> sessions,
            std::shared_ptr<sessions::SessionOpening> opening,
            std::shared_ptr<persistence::SaveService> saves,
            ProjectStorage& project,
            std::shared_ptr<persistence::WriteCoordinator> writes,
            std::shared_ptr<persistence::IArtifactStore> files,
            process::ExecutionRuntime& runtime,
            std::shared_ptr<persistence::SaveExecution> execution
        );
        ~ProjectContentSaving() override;
        ProjectContentSaving(const ProjectContentSaving&) = delete;
        ProjectContentSaving& operator=(const ProjectContentSaving&) = delete;
        ProjectContentSaving(ProjectContentSaving&&) = delete;
        ProjectContentSaving& operator=(ProjectContentSaving&&) = delete;

        // Used by a reviewed close: preparation does not admit encoding or change the source baseline.
        [[nodiscard]] EditorResult<PreparedProjectSave> prepare(
            sessions::ContentStamp,
            persistence::ESaveMode,
            std::string destination = {}
        );
        [[nodiscard]] EditorResult<persistence::SaveId> request(
            sessions::ContentStamp,
            persistence::ESaveMode,
            std::string destination = {}
        );
        // Transfer the observation responsibility for a SaveId already accepted by the existing fixed-set
        // close operation. On refusal that operation still owns it and must retain its destination values.
        [[nodiscard]] EditorResult<void> track(
            persistence::SaveId,
            std::span<const ProjectAssetEntry> reviewed_destinations = {}
        );
        [[nodiscard]] EditorResult<void> saveAll();
        // Borrowed close results remain in SaveService until the close owner releases its fixed set.
        [[nodiscard]] EditorResult<void> update(std::span<const sessions::SaveAllEntry> borrowed = {});
        [[nodiscard]] EditorResult<void> acknowledge(persistence::SaveId);
        [[nodiscard]] EditorResult<void> acknowledgeSaveAll();

        // Accept a fixed input for owner-stage admission. BUSY retains it; later source changes are
        // rejected, never silently recaptured. Accepted work survives all observing windows.
        [[nodiscard]] persistence::PersistenceResult<std::uint64_t>
        requestArtifact(persistence::DerivedArtifact) noexcept override;
        [[nodiscard]] EditorResult<std::vector<ArtifactPublicationReport>> artifactReports() const;
        [[nodiscard]] EditorResult<void> retryArtifact(std::uint64_t);
        [[nodiscard]] EditorResult<void> abandonArtifact(std::uint64_t);
        [[nodiscard]] EditorResult<void> acknowledgeArtifact(std::uint64_t);

        // Borrowed observations are valid until the next mutating call. They never expose catalog permits.
        [[nodiscard]] std::span<const ProjectSaveReport> reports() const noexcept;
        [[nodiscard]] std::span<const persistence::SaveId> pending() const noexcept;
        [[nodiscard]] std::span<const sessions::SaveAllEntry> saveAllEntries() const noexcept;
        [[nodiscard]] bool hasSaveAll() const noexcept;
        [[nodiscard]] bool hasCapacity(std::size_t additional) const noexcept;
        [[nodiscard]] bool settled() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
