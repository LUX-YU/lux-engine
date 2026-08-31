#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/function/render/client/RenderProgramSession.hpp>
#include <lux/engine/function/render/client/genops/LightOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/scene/runtime/render/visibility.h>
#include <lux/engine/simulation/ecs/Entity.hpp>
#include <lux/engine/simulation/ecs/Registry.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace lux::scene
{
    [[nodiscard]] constexpr render::RenderEntityId toRenderEntity(simulation::ecs::Entity entity) noexcept
    {
        return static_cast<render::RenderEntityId>(simulation::ecs::entityBits(entity));
    }

    enum class ERenderPublishResult : std::uint8_t
    {
        NoChanges,
        Published,
        Backpressured,
        FullSyncPublished,
        Failed,
    };

    enum class ERenderForwardResult : std::uint8_t
    {
        NoUpdate,
        Forwarded,
        Backpressured,
        Stopping,
    };

    enum class ERenderSystemError : std::uint8_t
    {
        InvalidConfiguration,
        AllocationFailure,
        EntityCapacityExceeded,
        SpatialEncodingFailure,
        ProgramEncodingFailure,
    };

    struct RenderSystemFailure final
    {
        ERenderSystemError code{};
        simulation::ecs::Entity entity{simulation::ecs::NullEntity};
    };

    class LUX_ENGINE_SCENE_RUNTIME_RENDER_PUBLIC RenderSystem final
    {
    public:
        struct Config final
        {
            render::RenderSceneId scene{};
            render::MeshStackOperationIds mesh_stack{};
            render::LightOperationIds light{};
            double coordinate_page_size{1024.0};
            std::array<std::int64_t, 3> scene_origin_page{};
            std::size_t expected_entity_capacity{65536U};
        };

        [[nodiscard]] static lux::cxx::expected<std::unique_ptr<RenderSystem>, RenderSystemFailure>
        create(simulation::ecs::Registry& registry, Config config) noexcept;

        ~RenderSystem() noexcept;
        RenderSystem(const RenderSystem&) = delete;
        RenderSystem& operator=(const RenderSystem&) = delete;
        RenderSystem(RenderSystem&&) = delete;
        RenderSystem& operator=(RenderSystem&&) = delete;

        [[nodiscard]] ERenderPublishResult tryPublish() noexcept;
        void requestFullSync() noexcept;
        [[nodiscard]] ERenderForwardResult tryForwardUpdate(render::RenderProgramSession& session) noexcept;

    private:
        struct Impl;
        explicit RenderSystem(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
    };
}
