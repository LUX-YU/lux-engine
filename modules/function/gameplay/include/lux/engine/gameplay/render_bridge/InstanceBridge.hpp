#pragma once
/**
 * @file InstanceBridge.hpp
 * @brief Generic bridge for INSTANCE components (Static/Skeletal Mesh): the full GPU
 *        mesh-instance lifecycle written once —
 *          - first sight : ensure mesh+material (async, cached), addMeshInstance,
 *                          refcount-acquire the assets once the slot reply lands;
 *          - steady state: detect an asset-id swap (tear down → rebuild next frame),
 *                          flip first-frame view visibility, diff+push instance flags
 *                          (cast-shadow / visible / highlight, single-writer), and
 *                          push the transform ONLY when dirty — zero-copy, the matrix
 *                          pointer comes straight from the component (§6.1);
 *          - reap        : drop instances whose entity left the view (died, shed C,
 *                          or gained an Exclude tag e.g. world-streaming dormant),
 *                          releasing asset refcounts (GPU resource freed at 0);
 *          - finalize    : optional per-frame batch flush (Skeletal's one-shot
 *                          uploadBoneBatch covering every skinned instance).
 *
 * EcsRenderTraits<C> supplies `geometry` / `Require` / `Exclude` / `transform`, plus
 * the optional skinning quartet `FrameState` / `beginFrame` / `accumulate` / `flush`
 * (detected with C++20 `requires` — a trait without them pays nothing). The component
 * fields `mesh_asset_id` / `material_asset_id` / `cast_shadow` / `visible` are read
 * directly (duck-typed: intrinsic to the INSTANCE category). Folds the lifecycle
 * helpers the old mesh adapters shared (steady-state swap / reap / instance-flags diff).
 */

#include <cstdint>
#include <unordered_map>
#include <utility>

#include <lux/engine/meta/LuxObject.hpp>                    // entity_id / EntityRegistry
#include <lux/engine/asset/Asset.hpp>                       // asset_id_t

#include <lux/engine/render/comm/RenderProtocol.hpp>        // MeshInstanceSlotReply / kInstanceFlag*
#include <lux/engine/render/core/RenderObjectTypes.hpp>     // RenderObjectHandle
#include <lux/engine/render/core/RenderResourceHandle.hpp>  // RMeshHandle
#include <lux/engine/render/comm/client/RenderSession.hpp>  // RenderSession (.then)
#include <lux/engine/render/comm/client/RenderRequest.hpp>  // in-flight create handle (cancel on teardown)
#include <lux/engine/render/renderer/features/meshstack/MeshStackOperation.hpp>  // MeshStackProxy
#include <lux/engine/render/renderer/features/highlight/HighlightOperation.hpp>  // kInstanceFlagHighlight

#include "lux/engine/gameplay/render_bridge/IRenderableBridge.hpp"
#include "lux/engine/gameplay/render_bridge/RenderableBridgeContext.hpp"
#include "lux/engine/gameplay/render_bridge/EcsRenderTraits.hpp"

namespace lux::gameplay
{
    namespace instance_detail
    {
        /// Empty stand-in when a trait declares no per-frame FrameState (static mesh).
        struct NoFrameState {};

        template <class T> struct FrameStateOf { using type = NoFrameState; };
        template <class T> requires requires { typename T::FrameState; }
        struct FrameStateOf<T> { using type = typename T::FrameState; };
    }

    template <class C>
    class InstanceBridge final : public IRenderableBridge
    {
        using T          = EcsRenderTraits<C>;
        using FrameState = typename instance_detail::FrameStateOf<T>::type;

        /// One live GPU instance. `mesh` is kept for the skinning batch (static
        /// ignores it); the asset ids let reap release refcounts after the source
        /// component is already gone.
        struct Live
        {
            lux::render::RenderObjectHandle object{};
            lux::render::RMeshHandle        mesh{};
            bool                            visible_for_view{false};
            lux::asset::asset_id_t          mesh_id{};
            lux::asset::asset_id_t          material_id{};
            std::uint32_t                   last_flags{0};
        };

        std::unordered_map<lux::meta::entity_id, Live> instances_;
        // addMeshInstance reply in flight, keyed by entity → the request handle.
        // We hold the handle (not just a flag) so the dtor can CANCEL any still-
        // pending create: its reply may be pumped after this bridge + its context
        // are destroyed (replies share one queue; an unrelated pumpReplies can
        // dispatch it), and the continuation captures both `this` AND `&ctx`
        // (acquireMesh/acquireMaterial). Cancel detaches the continuation so the
        // late reply is dropped; the GPU instance is reclaimed by the reap /
        // scene-destruction fallback.
        std::unordered_map<lux::meta::entity_id,
                           lux::render::RenderRequest<lux::render::MeshInstanceSlotReply>> pending_;
        FrameState                                     frame_{};   // per-frame skinning scratch (empty for static)

        /// Single writer of the per-instance flags word: cast-shadow / visible from the
        /// component plus the highlight bit, so selection rides the same updateInstanceFlags
        /// diff path (no second writer clobbering it).
        static std::uint32_t instanceFlags(const C& c, bool highlighted) noexcept
        {
            std::uint32_t f = lux::render::kInstanceFlagReceiveShadow;
            if (c.cast_shadow) f |= lux::render::kInstanceFlagCastShadow;
            if (c.visible)     f |= lux::render::kInstanceFlagVisible;
            if (highlighted)   f |= lux::render::kInstanceFlagHighlight;
            return f;
        }

    public:
        ~InstanceBridge() override
        {
            for (auto& [e, req] : pending_) req.cancel();
        }

        void drive(lux::meta::EntityRegistry& reg, RenderableBridgeContext& ctx) override
        {
            if constexpr (requires { T::beginFrame(frame_); }) T::beginFrame(frame_);

            auto mesh = ctx.meshStack();   // feature-scoped instance commands (by value, cheap)
            auto view = componentView<C>(reg, typename T::Require{}, typename T::Exclude{});
            view.each([&](lux::meta::entity_id e, C& c, auto&&...)
            {
                const InstanceTransform xf = T::transform(e, reg);   // zero-copy world matrix + dirty

                if (auto it = instances_.find(e); it != instances_.end())
                {
                    Live& inst = it->second;

                    // Runtime asset-id swap: tear down so next frame's first-sight path
                    // rebuilds with the new ids (one-frame gap).
                    if (inst.mesh_id != c.mesh_asset_id || inst.material_id != c.material_asset_id)
                    {
                        mesh.removeMeshInstance(ctx.scene(), inst.object);
                        ctx.releaseMesh(inst.mesh_id);
                        ctx.releaseMaterial(inst.material_id);
                        instances_.erase(it);
                        return;
                    }
                    if (!inst.visible_for_view)
                    {
                        mesh.makeInstanceVisibleForView(ctx.scene(), ctx.view(), inst.object);
                        inst.visible_for_view = true;
                    }
                    const std::uint32_t flags = instanceFlags(c, ctx.isHighlighted(e));
                    if (flags != inst.last_flags)
                    {
                        mesh.updateInstanceFlags(ctx.scene(), inst.object, flags);
                        inst.last_flags = flags;
                    }
                    if (xf.dirty)
                        mesh.updateTransform(inst.object, xf.world);   // §6.1 borrowed, not copied

                    if constexpr (requires { T::accumulate(frame_, inst.object, inst.mesh, e, reg); })
                        T::accumulate(frame_, inst.object, inst.mesh, e, reg);
                    return;
                }
                if (pending_.count(e)) return;   // create reply still in flight

                // First sight: resolve assets (async, cached) then fire the create.
                const lux::render::RMeshHandle m = ctx.ensureMesh(c.mesh_asset_id);
                if (m.is_null()) return;   // mesh not ready — retry next frame
                const lux::render::RMaterialHandle mat = ctx.ensureMaterial(c.material_asset_id);
                if (!c.material_asset_id.is_nil() && mat.is_null())
                    return;   // a material was named but isn't ready — wait so we create WITH it

                const std::uint32_t flags = instanceFlags(c, ctx.isHighlighted(e));
                // Capture asset ids so the reply can store them (the source component
                // may be gone by reap time).
                const auto mesh_id     = c.mesh_asset_id;
                const auto material_id = c.material_asset_id;
                auto req = mesh.addMeshInstance(ctx.scene(), m, mat, xf.world, flags, T::geometry);
                req.then(
                    [this, e, m, mesh_id, material_id, flags, ctx_ptr = &ctx](const lux::render::MeshInstanceSlotReply& r)
                    {
                        pending_.erase(e);                    // drops this request; the store keeps
                                                              // the state alive across this call
                        instances_.emplace(e, Live{ r.object, m, false, mesh_id, material_id, flags });
                        ctx_ptr->acquireMesh(mesh_id);
                        ctx_ptr->acquireMaterial(material_id);
                    });
                pending_.emplace(e, std::move(req));
            });
        }

        void finalize(RenderableBridgeContext& ctx) override
        {
            if constexpr (requires { T::flush(frame_, ctx); }) T::flush(frame_, ctx);
        }

        void reap(lux::meta::EntityRegistry& reg, RenderableBridgeContext& ctx) override
        {
            auto mesh = ctx.meshStack();
            for (auto it = instances_.begin(); it != instances_.end(); )
            {
                if (!inComponentView<C>(reg, it->first, typename T::Require{}, typename T::Exclude{}))
                {
                    mesh.removeMeshInstance(ctx.scene(), it->second.object);
                    ctx.releaseMesh(it->second.mesh_id);
                    ctx.releaseMaterial(it->second.material_id);
                    it = instances_.erase(it);
                }
                else ++it;
            }
        }
    };

} // namespace lux::gameplay
