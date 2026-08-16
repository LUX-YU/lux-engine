#pragma once
/**
 * @file MeshSubsystems.hpp
 * @brief MeshSubsystem / SkeletalMeshSubsystem —— 静态/骨骼网格的渲染子系统,
 *        「网格实例」形状(MeshInstanceSubsystem<各自的 RenderPolicy>). Each entity
 *        owns a GPU mesh instance whose full lifecycle
 *        (asset ensure/refcount, first-sight add, swap/visibility/flags/transform, reap)
 *        lives once in MeshInstanceSubsystem; the policies only declare the few differences:
 *          - `geometry`  : StaticMesh vs SkinnedMesh,
 *          - `Require`    : both need a ResolvedTransform3DComponent; skeletal also an Animator,
 *          - `Exclude`    : empty; unloaded World Actors are absent from ECS,
 *          - `transform` : zero-copy world matrix + dirty bit (points into the component),
 *          - skeletal adds the optional per-frame bone-palette batch (FrameState /
 *            beginFrame / accumulate / flush) flushed as one uploadBoneBatch.
 *        Replaces the two hand-written mesh bridges and their shared support helpers.
 */

#include <cstddef>   // std::byte
#include <cstdint>
#include <vector>

#include <lux/engine/function/render/client/resources/mesh/RenderObjectTypes.hpp>                       // EGeometryKind / RenderObjectHandle
#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>                    // RMeshHandle
#include <lux/engine/function/render/client/RenderFrameSession.hpp>                    // RenderFrameSession
#include <lux/engine/function/render/client/genops/SkinningOperation.ops.hpp> // BoneBatchEntry / SkinningProxy

#include "lux/engine/ecs/render/components/3d/MeshComponent.hpp"
#include "lux/engine/ecs/render/components/3d/SkeletalMeshComponent.hpp"
#include "lux/engine/ecs/render/components/3d/AnimatorComponent.hpp"
#include "lux/engine/ecs/components/ResolvedTransform3DComponent.hpp"
#include <lux/engine/ecs/render/RenderSpatialTransform.hpp>
#include "lux/engine/ecs/render/SceneRenderBinding.hpp"
#include "lux/engine/ecs/render/RenderViewUtil.hpp"                   // InstanceTransform / ComponentList
#include "lux/engine/ecs/render/subsystems/MeshInstanceSubsystem.hpp"

namespace lux::ecs
{
    // Component types live in the lux::ecs kit;
    // bring the ones these policies name into scope so the policy structs +
    // view companions read unqualified.

    namespace mesh_detail
    {
        /// Both meshes place themselves from a ResolvedTransform3DComponent; the matrix is
        /// borrowed (column-major Eigen storage == std430 mat4), never copied.
        inline InstanceTransform worldTransform(
            lux::meta::entity_id e,
            lux::meta::EntityRegistry& reg,
            SceneRenderBinding& render)
        {
            const auto& wt = reg.get<ResolvedTransform3DComponent>(e);
            const auto spatial = makeRenderSpatialTransform(
                wt,
                render.sceneOriginTile3D()
            );
            InstanceTransform result{};
            result.valid = spatial.has_value();
            if (spatial)
                result.spatial = *spatial;
            else
                render.requestSceneOriginRebase(wt.position);
            return result;
        }
    }

    /// MeshInstanceSubsystem 的静态网格策略(原 EcsRenderTraits<MeshComponent> 特化)。
    struct MeshRenderPolicy final
    {
        using Component = MeshComponent;
        static constexpr lux::render::EGeometryKind geometry = lux::render::EGeometryKind::StaticMesh;
        using Require = ComponentList<ResolvedTransform3DComponent>;
        using Exclude = ComponentList<>;

        static InstanceTransform transform(lux::meta::entity_id e, lux::meta::EntityRegistry& reg, SceneRenderBinding& render)
        {
            return mesh_detail::worldTransform(e, reg, render);
        }
    };

    /// MeshInstanceSubsystem 的骨骼网格策略(原 EcsRenderTraits<SkeletalMeshComponent> 特化)。
    struct SkeletalMeshRenderPolicy final
    {
        using Component = SkeletalMeshComponent;
        static constexpr lux::render::EGeometryKind geometry = lux::render::EGeometryKind::SkinnedMesh;
        using Require = ComponentList<ResolvedTransform3DComponent, AnimatorComponent>;   // + animated palette
        using Exclude = ComponentList<>;


        static InstanceTransform transform(lux::meta::entity_id e, lux::meta::EntityRegistry& reg, SceneRenderBinding& render)
        {
            return mesh_detail::worldTransform(e, reg, render);
        }

        // ── Optional per-frame skinning batch ──────────────────────────────
        // AnimationSystem (earlier in tick order) fills AnimatorComponent::
        // skinning_matrices. MeshInstanceSubsystem accumulates every visible skinned
        // instance's palette here during drive(), then flushes ONE uploadBoneBatch
        // in finalize() (after every subsystem's drive has run). Detected by the
        // base via `requires` — the static path has no FrameState, so it pays
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

        static void flush(FrameState& f, SceneRenderBinding& ctx)
        {
            // Feature-scoped skinning op-ids; SkinningProxy no-ops if the scene has
            // no SkinningFeature (ctx.skinningOps() invalid).
            if (f.entries.empty()) return;
            lux::render::UploadBoneBatchPayload wire{};
            wire.scene_id    = ctx.scene();
            wire.entry_count = static_cast<std::uint32_t>(f.entries.size());
            lux::render::SkinningProxy(ctx.session(), ctx.skinningOps())
                .uploadBoneBatch(wire,
                    std::as_bytes(std::span<const lux::render::BoneBatchEntry>(f.entries)),
                    alignof(lux::render::BoneBatchEntry),
                    std::span<const std::byte>(f.mats.data(), f.mats.size()),
                    16u /* mat4 对齐 */);
        }
    };

    /// 静态/骨骼网格的渲染子系统:「网格实例」形状(每实体一个 GPU 网格实例,
    /// 资产就绪 → add,换资产 → 拆重建,离场 → remove;骨骼版再加每帧骨骼批)。
    using MeshSubsystem         = MeshInstanceSubsystem<MeshRenderPolicy>;
    using SkeletalMeshSubsystem = MeshInstanceSubsystem<SkeletalMeshRenderPolicy>;

} // namespace lux::ecs
