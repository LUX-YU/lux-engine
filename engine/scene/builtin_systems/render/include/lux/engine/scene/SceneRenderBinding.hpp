#pragma once

#include <lux/engine/scene/RenderRuntime.hpp>
#include <lux/engine/scene/RenderSyncPipeline.hpp>
#include <lux/engine/scene/RenderSystemMetadata.hpp>

namespace lux::scene
{
    enum class ESceneRenderBindingState : std::uint8_t
    {
        CREATING,
        ATTACHING,
        READY,
        CLOSING,
        CLOSED,
        FAILED
    };

    struct SceneRenderBindingFailure final
    {
        SceneSystemBuildFailure scene;
        render::RenderError render;
    };

    // Main-only installation input. Binding outlives the installed Scene system.
    class LUX_ENGINE_SCENE_RENDER_PUBLIC SceneRenderInput final
    {
      public:
        ~SceneRenderInput();
        SceneRenderInput(SceneRenderInput &&) noexcept;
        SceneRenderInput &operator=(SceneRenderInput &&) noexcept;
        SceneRenderInput(const SceneRenderInput &) = delete;
        SceneRenderInput &operator=(const SceneRenderInput &) = delete;

        [[nodiscard]] render::RenderSceneId sceneId() const noexcept;
        [[nodiscard]] double coordinatePageSize() const noexcept;
        [[nodiscard]] lux::cxx::expected<std::unique_ptr<RenderSyncPipeline>, SceneSystemBuildFailure> makePipeline(
            simulation::ecs::Registry &registry, SceneSystemView description);

      private:
        friend class SceneRenderBinding;
        struct Data;
        explicit SceneRenderInput(std::unique_ptr<Data> data) noexcept;
        std::unique_ptr<Data> data_;
    };

    // Main-only owner of render scene creation, feature attachment and retained updates.
    // The existing renderer reply pump advances requests; poll never blocks or drains it.
    class LUX_ENGINE_SCENE_RENDER_PUBLIC SceneRenderBinding final
    {
      public:
        [[nodiscard]] static lux::cxx::expected<std::unique_ptr<SceneRenderBinding>, SceneRenderBindingFailure> begin(
            RenderRuntime &runtime, SceneSystemView description, std::shared_ptr<const RenderSystemMetadata> metadata);
        ~SceneRenderBinding();
        SceneRenderBinding(const SceneRenderBinding &) = delete;
        SceneRenderBinding &operator=(const SceneRenderBinding &) = delete;

        // Returns accepted Program submissions, including finite close-drain submissions.
        std::size_t poll(std::size_t packet_budget);
        [[nodiscard]] RenderSyncStatistics statistics() const noexcept;
        [[nodiscard]] ESceneRenderBindingState state() const noexcept;
        // Acceptance of the normal-close marker, not backend/GPU completion.
        [[nodiscard]] bool drainSubmitted() const noexcept;
        // Failure survives CLOSING/CLOSED; state() describes lifecycle progress.
        [[nodiscard]] bool hasFailure() const noexcept;
        [[nodiscard]] const SceneRenderBindingFailure &failure() const noexcept;
        [[nodiscard]] lux::cxx::expected<SceneRenderInput, SceneRenderBindingFailure> takeInput();
        [[nodiscard]] bool hasPendingUpdate() const noexcept;
        // Caller first closes views (CPU references and GPU watermark), stops and
        // destroys the producer. CLOSED records release publication, not GPU completion.
        void requestClose() noexcept;

      private:
        struct Data;
        explicit SceneRenderBinding(std::unique_ptr<Data> data) noexcept;
        std::unique_ptr<Data> data_;
    };
} // namespace lux::scene
