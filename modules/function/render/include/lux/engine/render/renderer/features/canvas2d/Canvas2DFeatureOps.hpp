#pragma once
// ============================================================================
//  Canvas2DFeatureOps.hpp — Canvas2DFeature factory + feature-scoped submit op.
//
//  The 2D draw-batch submit is a FEATURE-scoped dynamic op (the grid pattern),
//  not a core protocol op — mirrors LightOperation.hpp. The sprite list is a
//  Blob (variable length, no borrowed pointer across the frame); the owning
//  RenderSceneId rides the op envelope (G-04 per-command routing), NOT each
//  SpriteDraw (R2-00 kept scene_id out of the draw POD on purpose).
//
//  This is the COMM-coupled half of the Canvas2D protocol; the plain draw PODs
//  (DrawOrderKey / SpriteDraw / owner handles) stay in the lean Canvas2DOperation.hpp.
//  Handlers + factory definition live in Canvas2DOperationHandlers.cpp; the client
//  bridge that SENDS the op is wired in R2-04.
// ============================================================================

#include <lux/engine/render/renderer/features/canvas2d/Canvas2DOperation.hpp>  // SpriteDraw
#include <lux/engine/render/renderer/features/FeatureOps.hpp>                   // EOpKind / FeatureOpIds / TypeId
#include <lux/engine/render/comm/RenderCommTypes.hpp>                           // BlobRef
#include <lux/engine/render/core/RenderSceneId.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <span>
#include <type_traits>

namespace lux::render
{
    struct FeatureFactory;   // forward-decl (extern ref below) — avoids RenderProtocol.hpp here
    class RenderSession;

    /// Submit a per-frame batch of sprite draws to a scene's (single) Canvas2DFeature.
    /// Routed by scene_id (SinglePerScene → exactly one Canvas per scene, so no feature
    /// handle is needed); the sprites ride a Blob. Mirrors SetFeatureParamsPayload.
    struct Canvas2DSubmitPayload
    {
        RenderSceneId scene{};
        BlobRef       sprites{};   ///< raw bytes of a SpriteDraw[] in the frame payload
    };
    static_assert(std::is_trivially_copyable_v<Canvas2DSubmitPayload>);

    /// The single R2-01 op. Blob-kind → CommandOp opcode, pushBlob + push on the client,
    /// resolveBlob on the server. (Tilemap / pixel-field submit ops arrive with their slices.)
    struct Canvas2DSubmitOp
    {
        using Payload = Canvas2DSubmitPayload;
        static constexpr EOpKind      kind = EOpKind::Blob;
        static constexpr const char*  name = "Canvas2D.SubmitSprites";
    };

    /// Op ids returned to the client after RegisterFeatureType (forward-declarable, so
    /// gameplay bridges can stay render-light — mirrors LightOperationIds).
    struct Canvas2DOperationIds : FeatureOpIds<Canvas2DSubmitOp>
    {
        using Ids = FeatureOpIds<Canvas2DSubmitOp>;
        Canvas2DOperationIds() = default;
        Canvas2DOperationIds(const Ids& base) noexcept : Ids(base) {}
        [[nodiscard]] static Canvas2DOperationIds fromOps(const TypeId* ops, std::uint32_t count) noexcept
        {
            return Canvas2DOperationIds{Ids::fromOps(ops, count)};
        }
    };

    /// Canvas2DFeature factory (create + register_ops_fn allocating the submit op above).
    extern LUX_FUNCTION_PUBLIC const FeatureFactory kCanvas2DFeatureFactory;

    /// Client proxy — pushes a per-frame sprite batch to a scene's Canvas2DFeature.
    /// Wired to the Sprite2DBridge in R2-04; a fire-and-forget Blob command.
    class LUX_FUNCTION_PUBLIC Canvas2DProxy
    {
    public:
        Canvas2DProxy(RenderSession& session, Canvas2DOperationIds ops) noexcept
            : session_(&session), ops_(ops) {}

        /// Copies @p sprites into the frame blob and submits them. No-op if the batch is
        /// empty or the feature exposes no ops (unregistered).
        void submitSprites(RenderSceneId scene, std::span<const SpriteDraw> sprites);

        [[nodiscard]] bool valid() const noexcept { return ops_.valid(); }

    private:
        RenderSession*       session_;
        Canvas2DOperationIds ops_;
    };

} // namespace lux::render
