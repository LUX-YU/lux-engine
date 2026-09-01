#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/function/render/client/genops/LightOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/scene/RenderSyncStage.hpp>
#include <lux/engine/simulation/ecs/Registry.hpp>

#include <array>
#include <cstdint>
#include <memory>

namespace lux::scene
{
    enum class ERenderSyncStageError : std::uint8_t
    {
        INVALID_CONFIGURATION,
        ALLOCATION_FAILURE
    };

    struct RenderSyncStageFailure final
    {
        ERenderSyncStageError code{};
    };

    struct Mesh3DRenderStageConfig final
    {
        simulation::ecs::Registry& registry;
        render::RenderSceneId scene{};
        render::MeshStackOperationIds operations{};
        double coordinate_page_size{1024.0};
        std::array<std::int64_t, 3> scene_origin_page{};
    };

    struct Light3DRenderStageConfig final
    {
        simulation::ecs::Registry& registry;
        render::RenderSceneId scene{};
        render::LightOperationIds operations{};
        double coordinate_page_size{1024.0};
        std::array<std::int64_t, 3> scene_origin_page{};
    };

    [[nodiscard]] LUX_ENGINE_SCENE_RUNTIME_RENDER_PUBLIC
        lux::cxx::expected<std::unique_ptr<RenderSyncStage>, RenderSyncStageFailure>
        createMesh3DRenderStage(Mesh3DRenderStageConfig config) noexcept;

    [[nodiscard]] LUX_ENGINE_SCENE_RUNTIME_RENDER_PUBLIC
        lux::cxx::expected<std::unique_ptr<RenderSyncStage>, RenderSyncStageFailure>
        createLight3DRenderStage(Light3DRenderStageConfig config) noexcept;
} // namespace lux::scene
