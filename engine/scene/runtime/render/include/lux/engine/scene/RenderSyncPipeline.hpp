#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/function/render/client/RenderProgramSession.hpp>
#include <lux/engine/function/render/client/core/RenderEntityId.hpp>
#include <lux/engine/scene/RenderSyncStage.hpp>
#include <lux/engine/scene/runtime/render/visibility.h>
#include <lux/engine/simulation/ecs/Entity.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace lux::scene
{
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

    class LUX_ENGINE_SCENE_RUNTIME_RENDER_PUBLIC RenderSyncPipeline final
    {
    public:
        using StageList = std::vector<std::unique_ptr<RenderSyncStage>>;

        [[nodiscard]] static lux::cxx::expected<std::unique_ptr<RenderSyncPipeline>, RenderSyncPipelineFailure>
        create(StageList stages) noexcept;

        ~RenderSyncPipeline() noexcept;
        RenderSyncPipeline(const RenderSyncPipeline&) = delete;
        RenderSyncPipeline& operator=(const RenderSyncPipeline&) = delete;
        RenderSyncPipeline(RenderSyncPipeline&&) = delete;
        RenderSyncPipeline& operator=(RenderSyncPipeline&&) = delete;

        [[nodiscard]] ERenderPublishResult tryPublish() noexcept;
        void requestFullSync() noexcept;
        [[nodiscard]] ERenderForwardResult tryForwardUpdate(render::RenderProgramSession& session) noexcept;

    private:
        struct Impl;
        explicit RenderSyncPipeline(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::scene
