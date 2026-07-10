#pragma once
/**
 * @file GridRenderTraits.hpp
 * @brief EcsRenderTraits<GridComponent> — PARAM. Pushes the reference-grid params
 *        to the GridPass feature whenever a field changes. Replaces the hand-written Grid adapter.
 */

#include <optional>

#include <lux/engine/render/core/FeatureHandle.hpp>                       // FeatureHandle
#include <lux/engine/render/comm/client/RenderSession.hpp>               // RenderSession
#include <lux/engine/render/renderer/features/grid/GridOperation.hpp>    // GridProxy / GridOperationIds / GridSetParamsPayload

#include "lux/pack/d3/world/components/GridComponent.hpp"
#include "lux/engine/render_bridge/RenderableBridgeContext.hpp"
#include "lux/engine/render_bridge/EcsRenderTraits.hpp"

namespace lux::render_bridge
{
    using lux::pack::GridComponent;   // (a lux::pack component)

    template <>
    struct EcsRenderTraits<GridComponent>
    {
        static constexpr ERenderableKind kind    = ERenderableKind::PARAM;
        static constexpr const char*     feature = "GridPass";
        using Ops     = lux::render::GridOperationIds;
        using Payload = lux::render::GridSetParamsPayload;

        static std::optional<Payload> extract(const GridComponent& g, RenderableBridgeContext& ctx,
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
            return p;
        }

        static void push(lux::render::RenderSession& s, const Ops& ops, const Payload& p)
        {
            lux::render::GridProxy(s, ops).setParams(p.scene_id, p.feature,
                p.planeY, p.cellSize, p.linePx, p.fadeDist, p.holeRatio);
        }
    };

} // namespace lux::render_bridge
