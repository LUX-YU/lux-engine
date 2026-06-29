#pragma once
// ============================================================================
//  ViewCameraOperation.hpp — StandardViewCameraFeature factory + the
//  feature-scoped per-view camera update op + its payload.
//
//  The 3D camera (view/proj matrices, world camera position, derived frustum)
//  is a FEATURE domain. The per-frame camera update — which the core protocol
//  used to carry as the static ViewFrameUpdate BulkData op — is downloaded here:
//  the PAYLOAD, the op and the ViewCameraProxy live in this header, dispatched
//  by a dynamically allocated TypeId (register_ops_fn — the grid / light
//  pattern). The core RenderProtocol no longer names camera data; a 2D /
//  headless scene that omits StandardViewCamera carries none of this vocabulary.
//
//  Bulk kind: one push can carry several views' updates (the client sends a
//  span of 1 per view, mirroring the retired RenderSession::updateView). Each
//  entry is self-contained (scene_id + view) and resolves via lookupScene.
// ============================================================================

#include <lux/engine/render/comm/RenderProtocol.hpp>           // RenderSceneId / ViewHandle
#include <lux/engine/render/renderer/features/FeatureOps.hpp>  // EOpKind / FeatureOpIds
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <span>
#include <type_traits>

namespace lux::render
{
    struct FeatureFactory;
    class RenderSession;

    // =========================================================================
    //  Per-view camera update payload (moved out of core RenderProtocol.hpp —
    //  the core protocol no longer names camera data). Same shape as the retired
    //  ViewFrameUpdatePayload: column-major 4x4 matrices, Vulkan ZO projection.
    // =========================================================================
    struct ViewCameraUpdatePayload
    {
        RenderSceneId scene_id{};
        ViewHandle    view{};
        float view_matrix[16]{};      // column-major 4x4
        float proj_matrix[16]{};      // column-major 4x4, Vulkan ZO (NDC z in [0,1])
        float camera_position[3]{};   // world-space camera position
    };
    static_assert(std::is_trivially_copyable_v<ViewCameraUpdatePayload>);

    // =========================================================================
    //  Typed op — the single source the client Ids and the server registrar
    //  both derive from. Handler lives in ViewCameraOperationHandlers.cpp.
    // =========================================================================
    struct ViewCameraUpdateOp
    {
        using Payload = ViewCameraUpdatePayload;
        static constexpr EOpKind kind = EOpKind::Bulk;   // BulkData ring, no reply
        static constexpr const char* name = "ViewCameraUpdate";
    };

    /// Operation IDs returned to the client after RegisterFeatureType. A real
    /// (forward-declarable) struct rather than a `using` alias — gameplay headers
    /// forward-declare it to stay render-light; otherwise it simply IS a
    /// FeatureOpIds (look the op up by type via id<ViewCameraUpdateOp>()).
    struct ViewCameraOperationIds : FeatureOpIds<ViewCameraUpdateOp>
    {
        using Ids = FeatureOpIds<ViewCameraUpdateOp>;
        ViewCameraOperationIds() = default;
        ViewCameraOperationIds(const Ids& base) noexcept : Ids(base) {}
        [[nodiscard]] static ViewCameraOperationIds fromOps(const TypeId* ops, std::uint32_t count) noexcept
        {
            return ViewCameraOperationIds{Ids::fromOps(ops, count)};
        }
    };

    /// StandardViewCameraFeature factory (create + register_ops_fn allocating the
    /// camera update op). Adding it to a scene IS the opt-in to 3D camera data:
    /// the feature owns the per-scene ViewCameraResource (per-view matrices +
    /// frustum). A scene that omits it carries no camera vocabulary.
    ///
    /// MUST be added BEFORE every camera consumer (Forward / Deferred / MeshShadow
    /// / Hzb / SpatialCull / PointCloud / DeferredLighting), which read
    /// ViewCameraResource at their own attach / per frame — owner-first ordering,
    /// like StandardMeshStack.
    extern LUX_FUNCTION_PUBLIC const FeatureFactory kStandardViewCameraFeatureFactory;

    // =========================================================================
    //  Client-side proxy — ViewCameraProxy (sends per-view camera updates via the
    //  dynamic id). Replaces the retired RenderSession::updateView.
    // =========================================================================
    class LUX_FUNCTION_PUBLIC ViewCameraProxy
    {
    public:
        ViewCameraProxy(RenderSession& session, ViewCameraOperationIds ops) noexcept
            : session_(&session), ops_(ops) {}

        /// Push one view's per-frame camera data (column-major matrices).
        void update(RenderSceneId scene_id, ViewHandle view,
                    const float view_matrix[16],
                    const float proj_matrix[16],
                    const float camera_position[3]);

        [[nodiscard]] bool valid() const noexcept { return ops_.valid(); }

    private:
        RenderSession*         session_;
        ViewCameraOperationIds ops_;
    };

} // namespace lux::render
