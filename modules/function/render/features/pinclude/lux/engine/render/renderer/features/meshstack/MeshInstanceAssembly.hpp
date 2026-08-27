#pragma once

#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>

#include <span>

namespace lux::render
{
    class RenderContext;
    class RenderScene;
}

namespace lux::render::detail
{
    struct MeshInstanceRevision final
    {
        RenderObjectHandle object{};
        RMeshHandle mesh{};
        RMaterialHandle material{};
        RenderSpatialTransform3D transform{};
        std::uint32_t flags{0u};
        EGeometryKind geometry_kind{EGeometryKind::StaticMesh};
        PassMask pass_mask{kPassMaskOpaqueDefault};
        std::uint32_t user_meta_index{0u};
        std::uint32_t rgba8{0xffffffffu};
    };

    /// Render-thread-only assembly seam shared by the ordinary MeshStack wire
    /// handler and the World RenderCluster batch handler. Keeping the complete
    /// mesh-section/material/MDC construction behind this function prevents a
    /// second instance implementation from drifting away from MeshStack.
    [[nodiscard]] RenderObjectHandle createMeshInstance(
        void* server_state,
        RenderSceneId scene,
        RMeshHandle mesh,
        RMaterialHandle material,
        const RenderSpatialTransform3D& transform,
        std::uint32_t flags,
        EGeometryKind geometry_kind,
        PassMask pass_mask,
        std::uint32_t user_meta_index,
        bool visible_in_all_views,
        MeshInstanceCreateStatus& status,
        std::uint32_t transition_milliseconds = 0u,
        std::uint32_t transition_seed = 0u
    );

    /// Reconfigures existing live objects as one render-thread transaction.
    /// All mesh/material references, sections and MDC entries are prepared
    /// before any object is changed. A failure rolls the preparations back and
    /// leaves every old object and resource binding intact.
    [[nodiscard]] bool reconfigureMeshInstances(
        void* server_state,
        RenderSceneId scene,
        std::span<const MeshInstanceRevision> revisions,
        MeshInstanceCreateStatus& status
    );

    /// Symmetric render-thread destruction. It first unregisters the object
    /// from every live View, then returns all MeshStack/MDC slots.
    void destroyMeshInstance(void* server_state, RenderSceneId scene, RenderObjectHandle object) noexcept;

    /// Same destruction transaction for feature frame hooks, where the scene
    /// and render context are already resolved and no server shim is available.
    void destroyMeshInstance(RenderScene& scene, RenderContext& context, RenderObjectHandle object) noexcept;

    /// Back-fill a newly created View with an already-live object.
    void makeMeshInstanceVisible(
        void* server_state,
        RenderSceneId scene,
        std::uint32_t view_index,
        RenderObjectHandle object
    ) noexcept;

    void setMeshInstanceVisibility(
        void* server_state,
        RenderSceneId scene,
        RenderObjectHandle object,
        bool visible
    ) noexcept;
} // namespace lux::render::detail
