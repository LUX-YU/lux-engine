#pragma once
/**
 * @file MeshRenderTraits.hpp
 * @brief EcsRenderTraits for Static (MeshComponent) and Skeletal (SkeletalMeshComponent)
 *        meshes — INSTANCE. Each entity owns a GPU mesh instance whose full lifecycle
 *        (asset ensure/refcount, first-sight add, swap/visibility/flags/transform, reap)
 *        lives once in InstanceBridge; the traits only declare the few differences:
 *          - `geometry`  : StaticMesh vs SkinnedMesh,
 *          - `Require`    : both need a WorldTransformComponent; skeletal also an Animator,
 *          - `Exclude`    : world-streaming's RenderDormantComponent parks instances,
 *          - `transform` : zero-copy world matrix + dirty bit (points into the component),
 *          - skeletal adds the optional per-frame bone-palette batch (FrameState /
 *            beginFrame / accumulate / flush) flushed as one uploadBoneBatch.
 *        Replaces the two hand-written mesh bridges and their shared support helpers.
 */

#include <cstddef>   // std::byte
#include <cstdint>
#include <vector>

#include <lux/engine/render/core/RenderObjectTypes.hpp>                       // EGeometryKind / RenderObjectHandle
#include <lux/engine/render/core/RenderResourceHandle.hpp>                    // RMeshHandle
#include <lux/engine/render/comm/client/RenderSession.hpp>                    // RenderSession
#include <lux/engine/render/renderer/features/skinning/SkinningOperation.hpp> // BoneBatchEntry / SkinningProxy

#include "lux/pack/d3/world/components/MeshComponent.hpp"
#include "lux/pack/d3/world/components/SkeletalMeshComponent.hpp"
#include "lux/pack/d3/world/components/AnimatorComponent.hpp"
#include "lux/pack/d3/world/components/WorldTransformComponent.hpp"
#include "lux/pack/d3/world/components/RenderDormantComponent.hpp"
#include "lux/engine/render_bridge/RenderableBridgeContext.hpp"
#include "lux/engine/render_bridge/EcsRenderTraits.hpp"

namespace lux::render_bridge
{
    // Component types live in the lux::pack kit;
    // bring the ones these INSTANCE traits name into scope so the specializations +
    // view companions read unqualified.
    using lux::pack::MeshComponent;
    using lux::pack::SkeletalMeshComponent;
    using lux::pack::AnimatorComponent;
    using lux::pack::WorldTransformComponent;
    using lux::pack::RenderDormantComponent;

    namespace mesh_detail
    {
        /// Both meshes place themselves from a WorldTransformComponent; the matrix is
        /// borrowed (column-major Eigen storage == std430 mat4), never copied.
        inline InstanceTransform worldTransform(lux::meta::entity_id e, lux::meta::EntityRegistry& reg)
        {
            const auto& wt = reg.get<WorldTransformComponent>(e);
            return { wt.world.data(), wt.dirty };
        }
    }

    template <>
    struct EcsRenderTraits<MeshComponent>
    {
        static constexpr ERenderableKind            kind     = ERenderableKind::INSTANCE;
        static constexpr lux::render::EGeometryKind geometry = lux::render::EGeometryKind::StaticMesh;
        using Require = ComponentList<WorldTransformComponent>;
        using Exclude = ComponentList<RenderDormantComponent>;

        static InstanceTransform transform(lux::meta::entity_id e, lux::meta::EntityRegistry& reg)
        {
            return mesh_detail::worldTransform(e, reg);
        }
    };

    template <>
    struct EcsRenderTraits<SkeletalMeshComponent>
    {
        static constexpr ERenderableKind            kind     = ERenderableKind::INSTANCE;
        static constexpr lux::render::EGeometryKind geometry = lux::render::EGeometryKind::SkinnedMesh;
        using Require = ComponentList<WorldTransformComponent, AnimatorComponent>;   // + animated palette
        using Exclude = ComponentList<RenderDormantComponent>;

        static InstanceTransform transform(lux::meta::entity_id e, lux::meta::EntityRegistry& reg)
        {
            return mesh_detail::worldTransform(e, reg);
        }

        // ── Optional per-frame skinning batch ──────────────────────────────
        // AnimationSystem (earlier in tick order) fills AnimatorComponent::
        // skinning_matrices. InstanceBridge accumulates every visible skinned
        // instance's palette here during drive(), then flushes ONE uploadBoneBatch
        // in finalize() (after every bridge's drive has run). Detected by the
        // bridge via `requires` — the static path has no FrameState, so it pays
        // for none of this.
        struct FrameState
        {
            std::vector<lux::render::BoneBatchEntry> entries;
            std::vector<std::byte>                   mats;   // 64 B (one mat4) per bone
        };

        static void beginFrame(FrameState& f) noexcept
        {
            f.entries.clear();
            f.mats.clear();
        }

        static void accumulate(FrameState& f, lux::render::RenderObjectHandle object,
                               lux::render::RMeshHandle mesh, lux::meta::entity_id e,
                               lux::meta::EntityRegistry& reg)
        {
            // NOT gated on transform-dirty: the skinned output pool is a per-frame
            // transient arena, so every visible skinned instance must re-skin every
            // frame or its vertices vanish.
            const auto& anim = reg.get<AnimatorComponent>(e);
            if (anim.skinning_matrices.empty()) return;

            const auto bone_count  = static_cast<std::uint32_t>(anim.skinning_matrices.size());
            const auto bone_offset = static_cast<std::uint32_t>(f.mats.size() / 64u);
            const auto* src = reinterpret_cast<const std::byte*>(anim.skinning_matrices.data());
            f.mats.insert(f.mats.end(), src, src + static_cast<std::size_t>(bone_count) * 64u);
            f.entries.push_back(lux::render::BoneBatchEntry{ object, mesh, bone_offset, bone_count });
        }

        static void flush(FrameState& f, RenderableBridgeContext& ctx)
        {
            // Feature-scoped skinning op-ids; SkinningProxy no-ops if the scene has
            // no SkinningFeature (ctx.skinningOps() invalid).
            if (f.entries.empty()) return;
            lux::render::SkinningProxy(ctx.session(), ctx.skinningOps())
                .uploadBoneBatch(ctx.scene(), f.entries, f.mats.data(),
                                 static_cast<std::uint32_t>(f.mats.size() / 64u));
        }
    };

} // namespace lux::render_bridge
