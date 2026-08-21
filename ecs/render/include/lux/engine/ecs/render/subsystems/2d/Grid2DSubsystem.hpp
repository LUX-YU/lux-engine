#pragma once
/**
 * @file Grid2DSubsystem.hpp
 * @brief Grid2DSubsystem —— 2D 参考网格的渲染子系统,「特性参数」形状
 *        (FeatureParamSubsystem<Grid2DRenderPolicy>). Pushes the 2D reference-grid
 *        params to the Grid2DPass feature whenever a field changes (the
 *        Grid3DSubsystem sibling).
 */

#include <optional>

#include <lux/engine/function/render/client/core/FeatureHandle.hpp>
#include <lux/engine/function/render/client/RenderFrameSession.hpp>
#include <lux/engine/function/render/client/genops/Grid2DOperation.ops.hpp>   // 生成面(转包含作者头)

#include "lux/engine/ecs/render/components/2d/Grid2DComponent.hpp"
#include "lux/engine/ecs/render/SceneRenderBinding.hpp"
#include "lux/engine/ecs/render/subsystems/FeatureParamSubsystem.hpp"

namespace lux::ecs
{
    /// FeatureParamSubsystem 的 2D 网格策略(原 EcsRenderTraits<Grid2DComponent> 特化)。
    struct Grid2DRenderPolicy final
    {
        using Component = Grid2DComponent;
        static constexpr const char* feature = "Grid2DPass";
        using Ops     = lux::render::Grid2DOperationIds;
        using Payload = lux::render::Grid2DSetParamsPayload;

        static std::optional<Payload> extract(lux::ecs::Entity, const Grid2DComponent& g,
                                              lux::ecs::Registry&,
                                              SceneRenderBinding& ctx,
                                              lux::render::FeatureHandle feat)
        {
            Payload p{};
            p.scene_id   = ctx.scene();
            p.feature    = feat;
            p.cellSize   = g.cell_size;
            p.majorEvery = static_cast<float>(g.major_every);
            p.linePx     = g.line_px;
            p.onTop      = g.on_top ? 1u : 0u;
            return p;
        }

        static void push(lux::render::RenderFrameSession& s, const Ops& ops, const Payload& p)
        {
            // A+ 生成 Proxy 直接收 wire 载荷,拆装往返消失。
            lux::render::Grid2DProxy(s, ops).setParams(p);
        }
    };

    /// 2D 参考网格的渲染子系统:「特性参数」形状(整特性一份参数,extract → 脏比较 → push)。
    using Grid2DSubsystem = FeatureParamSubsystem<Grid2DRenderPolicy>;

} // namespace lux::ecs
