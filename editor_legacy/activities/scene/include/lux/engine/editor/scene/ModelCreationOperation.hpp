#pragma once
#include <lux/engine/editor/scene/ModelPlacement.hpp>
#include <lux/engine/editor/project/ProjectCatalogModel.hpp>
#include <lux/engine/process/asset_loading/AssetLoadSender.hpp>

namespace lux::editor::scene
{
    enum class EModelCreationStage : std::uint8_t
    {
        READING,
        READY,
        INSERTED,
        CANCELLED,
        FAILED
    };
    struct ModelCreationFailure final
    {
        using VCause = std::variant<
            SceneEditError,
            editing::EditFailure,
            project::VProjectQueryFailure,
            process::asset_loading::AssetLoadFailure,
            process::EExecutionError,
            process::TaskCancelled>;
        VCause cause;
    };
    template <class T> using ModelCreationResult = cxx::expected<T, ModelCreationFailure>;
    // One accepted model read and one atomic domain commit. Views never own its completion.
    class ModelCreationOperation final
    {
    public:
        [[nodiscard]] static ModelCreationResult<std::unique_ptr<ModelCreationOperation>> start(
            process::ExecutionRuntime&,
            sessions::TSessionAccess<SceneSession>,
            project::ProjectCatalogModel&,
            process::asset_loading::AssetReadPort,
            simulation::ecs::ComponentSchemaSet,
            ModelPlacement
        );
        ~ModelCreationOperation() noexcept;
        ModelCreationOperation(const ModelCreationOperation&) = delete;
        ModelCreationOperation& operator=(const ModelCreationOperation&) = delete;
        ModelCreationOperation(ModelCreationOperation&&) = delete;
        ModelCreationOperation& operator=(ModelCreationOperation&&) = delete;
        [[nodiscard]] EModelCreationStage stage() const noexcept;
        // Cancellation is an intent; the accepted transport must still deliver its owning result.
        [[nodiscard]] bool settled() const noexcept;
        [[nodiscard]] ModelCreationResult<SceneEditReceipt> commit();
        void cancel() noexcept;
        [[nodiscard]] const std::optional<ModelCreationFailure>& failure() const noexcept;

    private:
        struct Impl;
        explicit ModelCreationOperation(std::unique_ptr<Impl>) noexcept;
        std::unique_ptr<Impl> impl_;
    };
}
