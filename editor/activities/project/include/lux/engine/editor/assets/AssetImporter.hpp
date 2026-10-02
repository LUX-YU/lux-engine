#pragma once

#include <lux/engine/editor/EditorError.hpp>
#include <variant>
#include <optional>
#include <lux/engine/editor/assets/visibility.h>
#include <lux/engine/toolchain/asset/model/ModelCooker.hpp>

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
}

namespace lux::editor::assets
{
    enum class EAssetImportCloseState : std::uint8_t
    {
        OPEN,
        CLOSING,
        CLOSED
    };
    struct AssetImportCloseStatus final
    {
        EAssetImportCloseState state{EAssetImportCloseState::OPEN};
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

    struct AssetImportId final
    {
        std::uint64_t owner{}, serial{};
        friend bool operator==(AssetImportId, AssetImportId) = default;
    };

    enum class EAssetImportStage : std::uint8_t
    {
        READING,
        COOKING,
        WAITING_FOR_PROJECT,
        PUBLISHING,
        ABANDONING
    };
    struct AssetImportPending final
    {
        EAssetImportStage stage;
        std::size_t files{}, bytes{};
    };
    struct AssetImportSucceeded final
    {
        asset::AssetId asset;
        std::shared_ptr<const asset::ModelAsset> model;
        EditorResult<void> cleanup;
    };
    struct AssetImportAbandoned final
    {
        std::size_t published_files{};
    };
    using VAssetImportStatus =
        std::variant<AssetImportPending, EditorFailure, AssetImportSucceeded, AssetImportAbandoned>;

    // A project-bound import owner. It retains source bytes, compiled output and publication
    // effects through retry/close. No Window, Scene, or second asset catalog is owned here.
    class LUX_EDITOR_ASSETS_PUBLIC AssetImporter final
    {
    public:
        AssetImporter(ProjectStorage&, process::ExecutionRuntime&, persistence::WriteCoordinator&, persistence::IArtifactStore&, persistence::SaveExecution&);
        ~AssetImporter();
        AssetImporter(const AssetImporter&) = delete;
        AssetImporter(AssetImporter&&) = delete;
        AssetImporter& operator=(const AssetImporter&) = delete;
        AssetImporter& operator=(AssetImporter&&) = delete;

        [[nodiscard]] EditorResult<AssetImportId> requestModel(const ModelImportRequest&);
        [[nodiscard]] EditorResult<AssetImportId> reimportModel(
            asset::AssetId,
            const std::filesystem::path& replacement = {}
        );
        // Reattach a presenter without transferring ownership or losing a terminal result.
        [[nodiscard]] std::optional<AssetImportId> currentRequest() const noexcept;
        [[nodiscard]] EditorResult<VAssetImportStatus> status(AssetImportId) const;
        [[nodiscard]] EditorResult<void> retry(AssetImportId);
        [[nodiscard]] EditorResult<void> abandon(AssetImportId);
        [[nodiscard]] EditorResult<void> acknowledge(AssetImportId);
        void update() noexcept;
        void requestClose() noexcept;
        [[nodiscard]] AssetImportCloseStatus closeStatus() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor::assets
