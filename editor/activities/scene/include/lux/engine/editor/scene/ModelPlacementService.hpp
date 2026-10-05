#pragma once

#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/editor/scene/ModelCreationOperation.hpp>

namespace lux::services
{
    struct ServiceDescriptor;
}
namespace lux::editor
{
    class ProjectStorage;
}
namespace lux::editor::scene
{
    class ModelPlacementReport final
    {
    public:
        ModelPlacementReport(std::uint64_t, ModelPlacement);
        ~ModelPlacementReport();
        ModelPlacementReport(ModelPlacementReport&&) noexcept;
        ModelPlacementReport& operator=(ModelPlacementReport&&) noexcept;
        ModelPlacementReport(const ModelPlacementReport&) = delete;
        ModelPlacementReport& operator=(const ModelPlacementReport&) = delete;

        std::uint64_t id;
        ModelPlacement placement;
        std::optional<ModelCreationResult<SceneEditReceipt>> result;
        std::optional<EditorFailure> failure;
        bool cancel_requested{};
        [[nodiscard]] bool active() const noexcept;

    private:
        friend class ModelPlacementService;
        std::unique_ptr<ModelCreationOperation> operation_;
    };
    // Owns accepted placement inputs and results across view lifetimes. The original operation owns
    // transport completion and commits through the captured SceneSession gate exactly once.
    class ModelPlacementService final
    {
    public:
        ModelPlacementService(
            process::ExecutionRuntime&,
            sessions::TSessionAccess<SceneSession>,
            ProjectStorage&,
            simulation::ecs::ComponentSchemaSet,
            std::size_t capacity = 32
        );
        ~ModelPlacementService();
        ModelPlacementService(const ModelPlacementService&) = delete;
        ModelPlacementService& operator=(const ModelPlacementService&) = delete;
        ModelPlacementService(ModelPlacementService&&) = delete;
        ModelPlacementService& operator=(ModelPlacementService&&) = delete;

        // Admission only. No IO or author mutation occurs in an input callback.
        [[nodiscard]] EditorResult<std::uint64_t> request(ModelPlacement);
        [[nodiscard]] EditorResult<void> cancel(std::uint64_t) noexcept;
        [[nodiscard]] EditorResult<void> acknowledge(std::uint64_t) noexcept;
        [[nodiscard]] EditorResult<void> update() noexcept;
        [[nodiscard]] EditorResult<void> requestClose() noexcept;
        [[nodiscard]] bool settled() const noexcept;
        // Owner-thread observations; valid until the next mutating call, never a writable operation.
        [[nodiscard]] std::span<const ModelPlacementReport> reports() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
    extern const services::ServiceDescriptor kModelPlacementService;
} // namespace lux::editor::scene
