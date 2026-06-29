#include <lux/engine/editor/scene/systems/SelectionSceneSystem.hpp>

#include <lux/engine/editor/app/LuxEditor.hpp>   // EditorRenderInfra (feature_registry)
#include <lux/engine/editor/app/Selection.hpp>   // Selection::entity

#include <lux/engine/gameplay/world/World.hpp>
#include <lux/engine/gameplay/render_bridge/RenderableSystem.hpp>          // setHighlighted / anchor
#include <lux/engine/gameplay/world/HierarchyView.hpp>                    // subtree walk (highlight whole object)
#include <lux/engine/gameplay/DebugDraw.hpp>                               // debugdraw::lines()

#include <lux/engine/ui/UIRenderSession.hpp>                // ctx.session (RenderSession base)
#include <lux/engine/render/comm/client/RenderSession.hpp>  // LineListProxy upcast target
#include <lux/engine/render/renderer/features/gizmo/GizmoVertex.hpp>
#include <lux/engine/render/renderer/features/gizmo/LineListOperation.hpp>

#include <span>
#include <unordered_set>
#include <vector>

namespace lux::editor
{
    SelectionSceneSystem::SelectionSceneSystem()  = default;
    SelectionSceneSystem::~SelectionSceneSystem() = default;

    // -------------------------------------------------------------------------
    void SelectionSceneSystem::onPreRenderableUpdate(const SceneTickContext& ctx)
    {
        auto& reg = ctx.world.registry();
        std::unordered_set<lux::meta::entity_id> selected;

        const lux::meta::entity_id sel =
            ctx.selection ? ctx.selection->entity() : lux::meta::null_entity;
        if (sel != lux::meta::null_entity && reg.valid(sel))
        {
            // Highlight the WHOLE object: the selected root plus every descendant
            // mesh entity (W1-B promotes picks to the root, which may itself be
            // meshless). HierarchyView walks the subtree cycle-safely; the mesh
            // bridges fold kInstanceFlagHighlight into any instance whose entity is
            // in this set.
            const lux::gameplay::HierarchyView hierarchy(reg);
            hierarchy.forEachInSubtree(sel,
                [&](lux::meta::entity_id e) { selected.insert(e); });
        }

        // Empty set clears the highlight. Published every frame (cheap; the adapters'
        // last_flags diff suppresses redundant updateInstanceFlags uploads). The
        // editor's selection is the client that drives the render-side highlight.
        ctx.renderable.setHighlighted(std::move(selected));
    }

    // -------------------------------------------------------------------------
    void SelectionSceneSystem::onPostRenderableUpdate(const SceneTickContext& ctx)
    {
        // Build the proxy lazily; ops / scene_id stabilize before the first tick and
        // stay valid for the editor's lifetime.
        if (!line_list_proxy_)
            line_list_proxy_ = std::make_unique<lux::render::LineListProxy>(
                ctx.session,
                ctx.infra.feature_registry
                    .ops<lux::render::LineListOperationIds>("LineListTransient"));

        // The selection highlight is a render-side soft outline (HighlightFeature,
        // driven by kInstanceFlagHighlight) — no AABB wireframe is appended here.
        // This upload carries only script debug lines (lux_debug_draw_line):
        // retained CPU-side, so a one-shot script call keeps rendering until cleared.
        std::vector<lux::render::GizmoVertex> verts;
        for (const auto& line : lux::gameplay::debugdraw::lines())
        {
            verts.push_back(lux::render::GizmoVertex::make(
                line.from[0], line.from[1], line.from[2],
                line.color[0], line.color[1], line.color[2]));
            verts.push_back(lux::render::GizmoVertex::make(
                line.to[0], line.to[1], line.to[2],
                line.color[0], line.color[1], line.color[2]));
        }

        // THE editor's single line-list upload per frame. The server-side transient
        // buffer is last-writer-wins (chunk_id is ignored) — a second uploadLines
        // call anywhere this frame would silently erase everything above. New line
        // sources must MERGE here, never upload on their own. Empty vector = clear.
        line_list_proxy_->uploadLines(ctx.scene_id, /*chunk_id=*/0u,
            std::span<const lux::render::GizmoVertex>(verts.data(), verts.size()));
    }

} // namespace lux::editor
