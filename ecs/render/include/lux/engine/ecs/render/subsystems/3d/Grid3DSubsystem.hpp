#pragma once
/**
 * @file Grid3DSubsystem.hpp
 * @brief Grid3DSubsystem —— 3D 参考网格的渲染子系统,「特性参数」形状
 *        (FeatureParamSubsystem<Grid3DRenderPolicy>). Pushes the reference-grid params
 *        to the GridPass feature whenever a field changes. Replaces the hand-written Grid adapter.
 */

#include <optional>

#include <lux/engine/function/render/client/core/FeatureHandle.hpp>                       // FeatureHandle
#include <lux/engine/function/render/client/RenderFrameSession.hpp>               // RenderFrameSession
#include <lux/engine/function/render/client/genops/Grid3DOperation.ops.hpp>          // 生成面:Grid3DProxy / OperationIds(转包含作者头的载荷)

#include "lux/engine/ecs/render/components/3d/Grid3DComponent.hpp"
#include "lux/engine/ecs/render/SceneRenderBinding.hpp"
#include "lux/engine/ecs/render/subsystems/FeatureParamSubsystem.hpp"

namespace lux::ecs
{

    /// FeatureParamSubsystem 的 3D 网格策略(原 EcsRenderTraits<Grid3DComponent> 特化)。
    struct Grid3DRenderPolicy final
    {
        using Component = Grid3DComponent;
        static constexpr const char* feature = "Grid3DPass";
        using Ops     = lux::render::Grid3DOperationIds;
        using Payload = lux::render::Grid3DSetParamsPayload;

        static std::optional<Payload> extract(lux::meta::entity_id, const Grid3DComponent& g,
                                              lux::meta::EntityRegistry&,
                                              SceneRenderBinding& ctx,
                                              lux::render::FeatureHandle feat)
        {
            Payload p{};
            p.scene_id  = ctx.scene();
            p.feature   = feat;
            p.planeY    = g.plane_y;
            p.cellSize  = g.cell_size;
            p.linePx    = g.line_px;
            p.fadeDist  = g.fade_dist;
            p.holeRatio = g.hole_ratio;
            p.onTop     = g.on_top ? 1u : 0u;
            return p;
        }

        static void push(lux::render::RenderFrameSession& s, const Ops& ops, const Payload& p)
        {
            // A+ 生成 Proxy 直接收 wire 载荷 —— extract 装好的 p 原样进线,
            // 原先「拆成 8 参再装回」的往返消失。
            lux::render::Grid3DProxy(s, ops).setParams(p);
        }
    };

    /// 3D 参考网格的渲染子系统:「特性参数」形状(整特性一份参数,extract → 脏比较 → push)。
    using Grid3DSubsystem = FeatureParamSubsystem<Grid3DRenderPolicy>;

} // namespace lux::ecs
