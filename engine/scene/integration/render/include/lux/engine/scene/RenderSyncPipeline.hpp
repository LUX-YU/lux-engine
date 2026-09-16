#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/function/render/client/RenderProgramSession.hpp>
#include <lux/engine/function/render/client/core/RenderEntityId.hpp>
#include <lux/engine/scene/RenderSyncStage.hpp>
#include <lux/engine/scene/render/visibility.h>
#include <lux/engine/simulation/ecs/Entity.hpp>

#include <cstdint>
#include <memory>
#include <stop_token>
#include <vector>

namespace lux::scene
{
    namespace detail
    {
        struct RenderSyncStorage;
    }
    class SceneMetaManager;
    class SceneRenderInput;
    class SceneRenderBinding;
    class RenderSystem;
    [[nodiscard]] constexpr render::RenderEntityId toRenderEntity(simulation::ecs::Entity entity) noexcept
    {
        return static_cast<render::RenderEntityId>(simulation::ecs::entityBits(entity));
    }

    enum class ERenderPublishResult : std::uint8_t
    {
        NO_CHANGES,
        PUBLISHED,
        BACKPRESSURED,
        FULL_SYNC_PUBLISHED,
        FAILED
    };

    enum class ERenderForwardResult : std::uint8_t
    {
        NO_UPDATE,
        FORWARDED,
        BACKPRESSURED,
        STOPPING
    };

    enum class ERenderSyncPipelineError : std::uint8_t
    {
        INVALID_STAGE_LIST,
        ALLOCATION_FAILURE,
        STAGE_PREPARE_FAILURE,
        PROGRAM_ENCODING_FAILURE
    };

    struct RenderSyncPipelineFailure final
    {
        ERenderSyncPipelineError code{};
    };

    struct RenderSyncStatistics final
    {
        std::uint64_t published{}; // Accepted into the Scene-to-Main ring.
        std::uint64_t forwarded{}; // Accepted by Main's Program client; not GPU completion.
        std::uint64_t backpressured{};
        std::uint32_t pending{};
        std::uint32_t high_water{};
        std::uint64_t retired_unforwarded{}; // Terminal retirement; never
                                             // counted as forwarded.
    };

    // Main owns this endpoint. It never accesses stages or a Scene Registry.
    // FORWARDED means Program client acceptance, not renderer adoption or GPU completion.
    class LUX_ENGINE_SCENE_RENDER_PUBLIC RenderSyncConsumer final
    {
      public:
        ~RenderSyncConsumer();
        RenderSyncConsumer(RenderSyncConsumer &&) noexcept;
        RenderSyncConsumer &operator=(RenderSyncConsumer &&) noexcept;
        RenderSyncConsumer(const RenderSyncConsumer &) = delete;
        RenderSyncConsumer &operator=(const RenderSyncConsumer &) = delete;

        [[nodiscard]] ERenderForwardResult tryForwardUpdate(render::RenderProgramSession &session) noexcept;
        [[nodiscard]] bool hasPendingUpdate() const noexcept;
        [[nodiscard]] bool producerClosed() const noexcept;

      private:
        friend class RenderSyncPipeline;
        friend class SceneRenderBinding;
        explicit RenderSyncConsumer(std::shared_ptr<detail::RenderSyncStorage> storage) noexcept;
        void close() noexcept;
        void stop() noexcept;
        void retireAfterBackendStopped() noexcept;
        std::shared_ptr<detail::RenderSyncStorage> storage_;
        bool forward_pending_{};
    };

    class LUX_ENGINE_SCENE_RENDER_PUBLIC RenderSyncPipeline final
    {
    public:
        using StageList = std::vector<std::unique_ptr<RenderSyncStage>>;

        ~RenderSyncPipeline() noexcept;
        RenderSyncPipeline(const RenderSyncPipeline&) = delete;
        RenderSyncPipeline& operator=(const RenderSyncPipeline&) = delete;
        RenderSyncPipeline(RenderSyncPipeline&&) = delete;
        RenderSyncPipeline& operator=(RenderSyncPipeline&&) = delete;

        [[nodiscard]] ERenderPublishResult tryPublish() noexcept;
        void requestFullSync() noexcept;
        // Scene owner only. Stop interrupts even when Main cannot consume.
        [[nodiscard]] bool waitForCapacity(std::stop_token stop) const noexcept;

      private:
        friend class SceneRenderInput;
        [[nodiscard]] static lux::cxx::expected<std::unique_ptr<RenderSyncPipeline>, RenderSyncPipelineFailure> create(
            StageList stages, std::shared_ptr<detail::RenderSyncStorage> storage,
            std::shared_ptr<const SceneMetaManager> metadata);
        struct Impl;
        explicit RenderSyncPipeline(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::scene
