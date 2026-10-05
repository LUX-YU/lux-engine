#pragma once

#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/editor/assets/visibility.h>
#include <lux/engine/toolchain/asset/model/ModelCooker.hpp>
#include <optional>
#include <variant>

namespace lux::services
{
    struct ServiceDescriptor;
}
namespace lux::process
{
    class ExecutionRuntime;
}
namespace lux::editor
{
    class ProjectStorage;
}

namespace lux::editor::persistence
{
    class WriteCoordinator;
    class IArtifactStore;
    class SaveExecution;
} // namespace lux::editor::persistence

namespace lux::editor::assets
{
    extern const services::ServiceDescriptor kModelImporterService;

    enum class EModelImportCloseState : std::uint8_t
    {
        OPEN,
        CLOSING,
        CLOSED
    };
    struct ModelImportCloseStatus final
    {
        EModelImportCloseState state{EModelImportCloseState::OPEN};
        std::string waiting_for;
        EditorResult<void> progress;
    };
    struct ModelImportRequest final
    {
        asset::AssetId asset;
        std::filesystem::path file;
        std::string browser_path;
        lux::toolchain::ModelCookConfiguration configuration;
    };

    struct ModelImportId final
    {
        std::uint64_t owner{}, serial{};
        friend bool operator==(ModelImportId, ModelImportId) = default;
    };

    enum class EModelImportStage : std::uint8_t
    {
        READING,
        COOKING,
        WAITING_FOR_PROJECT,
        PUBLISHING,
        ABANDONING
    };
    struct ModelImportPending final
    {
        EModelImportStage stage;
        std::size_t files{}, bytes{};
    };
    struct ModelImportSucceeded final
    {
        asset::AssetId asset;
        std::shared_ptr<const asset::ModelAsset> model;
        EditorResult<void> cleanup;
    };
    struct ModelImportAbandoned final
    {
        std::size_t published_files{};
    };
    using VModelImportStatus =
        std::variant<ModelImportPending, EditorFailure, ModelImportSucceeded, ModelImportAbandoned>;

    // A project-bound import owner. It retains source bytes, compiled output and publication
    // effects through retry/close. No Window, Scene, or second asset catalog is owned here.
    class LUX_EDITOR_ASSETS_PUBLIC ModelImporter final
    {
    public:
        ModelImporter(ProjectStorage&, process::ExecutionRuntime&, persistence::WriteCoordinator&, persistence::IArtifactStore&, persistence::SaveExecution&);
        ModelImporter(ProjectStorage&, process::ExecutionRuntime&, std::shared_ptr<persistence::WriteCoordinator>, std::shared_ptr<persistence::IArtifactStore>, std::shared_ptr<persistence::SaveExecution>);
        ~ModelImporter();
        ModelImporter(const ModelImporter&) = delete;
        ModelImporter(ModelImporter&&) = delete;
        ModelImporter& operator=(const ModelImporter&) = delete;
        ModelImporter& operator=(ModelImporter&&) = delete;

        [[nodiscard]] EditorResult<ModelImportId> requestModel(const ModelImportRequest&);
        [[nodiscard]] EditorResult<ModelImportId> reimportModel(
            asset::AssetId,
            const std::filesystem::path& replacement = {}
        );
        // Reattach a presenter without transferring ownership or losing a terminal result.
        [[nodiscard]] std::optional<ModelImportId> currentRequest() const noexcept;
        [[nodiscard]] EditorResult<VModelImportStatus> status(ModelImportId) const;
        [[nodiscard]] EditorResult<void> retry(ModelImportId);
        [[nodiscard]] EditorResult<void> abandon(ModelImportId);
        [[nodiscard]] EditorResult<void> acknowledge(ModelImportId);
        void update() noexcept;
        void requestClose() noexcept;
        [[nodiscard]] ModelImportCloseStatus closeStatus() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor::assets
