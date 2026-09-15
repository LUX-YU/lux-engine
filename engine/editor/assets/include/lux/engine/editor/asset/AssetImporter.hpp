#pragma once

#include <lux/engine/editor/DocumentRequests.hpp>
#include <lux/engine/editor/assets/visibility.h>
#include <lux/engine/toolchain/asset/model/ModelCooker.hpp>

namespace lux::process
{
    class ExecutionRuntime;
}
namespace lux::editor
{
    class Project;
}

namespace lux::editor::assets
{
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
    };
    using AssetImportStatus =
        std::variant<AssetImportPending, EditorFailure, AssetImportSucceeded, AssetImportAbandoned>;

    // A project-bound import owner. It retains source bytes, compiled output and publication
    // effects through retry/close. No Window, Scene, or second asset catalog is owned here.
    class LUX_EDITOR_ASSETS_PUBLIC AssetImporter final
    {
      public:
        AssetImporter(Project &, process::ExecutionRuntime &);
        ~AssetImporter();
        AssetImporter(const AssetImporter &) = delete;
        AssetImporter(AssetImporter &&) = delete;

        [[nodiscard]] EditorResult<AssetImportId> requestModel(const ModelImportRequest &);
        [[nodiscard]] EditorResult<AssetImportId> reimportModel(asset::AssetId,
                                                                const std::filesystem::path &replacement = {});
        [[nodiscard]] EditorResult<AssetImportStatus> status(AssetImportId) const;
        [[nodiscard]] EditorResult<void> retry(AssetImportId);
        [[nodiscard]] EditorResult<void> abandon(AssetImportId);
        [[nodiscard]] EditorResult<void> acknowledge(AssetImportId);
        void poll(PollBudget &);
        void requestClose() noexcept;
        [[nodiscard]] CloseStatus closeStatus() const;

      private:
        struct Data;
        std::unique_ptr<Data> data_;
    };
} // namespace lux::editor::assets
