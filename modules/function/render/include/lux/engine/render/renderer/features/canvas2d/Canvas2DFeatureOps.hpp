#pragma once
// ============================================================================
//  Canvas2DFeatureOps.hpp — Canvas2DFeature factory + the v2 GPU-DRIVEN
//  feature-scoped instance ops (C2D-R0; .internal/2d-gpu-driven-rewrite.md).
//
//  Sprites are GPU-resident instances in the scene's Canvas2D arena. The wire
//  carries create / destroy / delta COMMANDS — never per-frame content:
//    AddSprite2D            Stream + reply (handle, G-05 status)   entity appears
//    RemoveSprite2D         Stream                                 entity dies
//    Sprite2DTransformBatch Bulk (per-entry scene routing, G-04)   per frame, DIRTY only
//    UpdateSprite2DVisual   Stream                                 uv/tint/texture changed
//    UpdateSprite2DKey      Stream                                 priority/visible changed
//    SetCanvas2DEnabled     Stream                                 camera gate flipped
//  A static scene therefore sends NOTHING (no heartbeat, no producer staging —
//  that whole v1 concept group is gone).
//
//  This is the COMM-coupled half; the comm-free PODs (Sprite2DInstanceData /
//  Sprite2DHandle / quantizePriority / status) stay in Canvas2DOperation.hpp.
//  Handlers + factory definition live in Canvas2DOperationHandlers.cpp; the
//  retained gameplay bridge that SENDS these ops is wired in C2D-R3.
// ============================================================================

#include <lux/engine/render/renderer/features/canvas2d/Canvas2DOperation.hpp>  // Sprite2DInstanceData / Sprite2DHandle / status
#include <lux/engine/render/renderer/features/FeatureOps.hpp>                   // EOpKind / FeatureOpIds / reply_type_id_of_v
#include <lux/engine/render/comm/RenderCommTypes.hpp>                           // CommandTraits / TypeId
#include <lux/engine/render/core/RenderSceneId.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <span>
#include <type_traits>

namespace lux::render
{
    struct FeatureFactory;   // forward-decl (extern ref below) — avoids RenderProtocol.hpp here
    class RenderSession;
    template <typename T> class RenderRequest;

    // ── v2 payloads ───────────────────────────────────────────────────────────

    /// Create one GPU-resident sprite instance. Replies with its owner handle +
    /// outcome (G-05: anything but Ok ⇒ handle null, nothing was created).
    struct AddSprite2DPayload
    {
        RenderSceneId        scene{};
        Sprite2DInstanceData data{};
        float                priority{0.f};
        std::uint32_t        visible{1};
    };
    static_assert(std::is_trivially_copyable_v<AddSprite2DPayload>);

    struct Sprite2DSlotReply
    {
        Sprite2DHandle        handle{};
        ECanvas2DCreateStatus status{ECanvas2DCreateStatus::Unknown};
    };
    static_assert(std::is_trivially_copyable_v<Sprite2DSlotReply>);

    template <>
    struct CommandTraits<AddSprite2DPayload>
    {
        using Reply = Sprite2DSlotReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = reply_type_id_of_v<Sprite2DSlotReply>;
    };

    struct RemoveSprite2DPayload
    {
        RenderSceneId  scene{};
        Sprite2DHandle handle{};
    };
    static_assert(std::is_trivially_copyable_v<RemoveSprite2DPayload>);

    /// Per-frame transform delta (BulkData). Each entry self-routes by its own
    /// `scene` (G-04); only DIRTY sprites produce entries, so wire traffic is
    /// proportional to change, never to scene size.
    struct Sprite2DTransformEntry
    {
        RenderSceneId  scene{};
        Sprite2DHandle handle{};
        float          m[6]{};   ///< column-major 2D affine (see Sprite2DInstanceData)
    };
    static_assert(std::is_trivially_copyable_v<Sprite2DTransformEntry>);

    /// Visual fields (uv rect / tint / texture) — low-frequency, order-neutral.
    struct UpdateSprite2DVisualPayload
    {
        RenderSceneId  scene{};
        Sprite2DHandle handle{};
        float          uv[4]{0.f, 0.f, 1.f, 1.f};
        std::uint32_t  tint{0xFFFFFFFFu};
        std::uint32_t  texture_bindless{kNoTexture};
    };
    static_assert(std::is_trivially_copyable_v<UpdateSprite2DVisualPayload>);

    /// Order-affecting fields — the ONLY op that can trigger an order rebuild.
    struct UpdateSprite2DKeyPayload
    {
        RenderSceneId  scene{};
        Sprite2DHandle handle{};
        float          priority{0.f};
        std::uint32_t  visible{1};
    };
    static_assert(std::is_trivially_copyable_v<UpdateSprite2DKeyPayload>);

    /// Scene-level draw gate (retained bit). The camera bridge flips it when the
    /// publishable-camera gate changes — the P0-5 concern ("never draw against a
    /// stale camera") expressed as ONE retained bit instead of per-frame liveness.
    struct SetCanvas2DEnabledPayload
    {
        RenderSceneId scene{};
        std::uint32_t enabled{1};
    };
    static_assert(std::is_trivially_copyable_v<SetCanvas2DEnabledPayload>);

    // ── PixelField kind payloads (F2-09; same command grammar as sprites) ─────

    struct AddPixelField2DPayload
    {
        RenderSceneId            scene{};
        PixelField2DInstanceData data{};
        float                    priority{0.f};
        std::uint32_t            visible{1};
    };
    static_assert(std::is_trivially_copyable_v<AddPixelField2DPayload>);

    struct PixelFieldSlotReply
    {
        PixelFieldInstanceHandle handle{};
        ECanvas2DCreateStatus    status{ECanvas2DCreateStatus::Unknown};
    };
    static_assert(std::is_trivially_copyable_v<PixelFieldSlotReply>);

    template <>
    struct CommandTraits<AddPixelField2DPayload>
    {
        using Reply = PixelFieldSlotReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = reply_type_id_of_v<PixelFieldSlotReply>;
    };

    struct RemovePixelField2DPayload
    {
        RenderSceneId            scene{};
        PixelFieldInstanceHandle handle{};
    };
    static_assert(std::is_trivially_copyable_v<RemovePixelField2DPayload>);

    /// Fields are FEW (a handful per scene) and mostly static — a plain Stream op
    /// per moved field beats a bulk lane here.
    struct UpdatePixelField2DTransformPayload
    {
        RenderSceneId            scene{};
        PixelFieldInstanceHandle handle{};
        float                    m[6]{};
    };
    static_assert(std::is_trivially_copyable_v<UpdatePixelField2DTransformPayload>);

    struct UpdatePixelField2DKeyPayload
    {
        RenderSceneId            scene{};
        PixelFieldInstanceHandle handle{};
        float                    priority{0.f};
        std::uint32_t            visible{1};
    };
    static_assert(std::is_trivially_copyable_v<UpdatePixelField2DKeyPayload>);

    // ── Tile kind payloads (A2-02; same command grammar as sprites/fields) ────

    struct AddTile2DPayload
    {
        RenderSceneId      scene{};
        Tile2DInstanceData data{};
        float              priority{0.f};
        std::uint32_t      visible{1};
    };
    static_assert(std::is_trivially_copyable_v<AddTile2DPayload>);

    struct Tile2DSlotReply
    {
        Tile2DInstanceHandle  handle{};
        ECanvas2DCreateStatus status{ECanvas2DCreateStatus::Unknown};
    };
    static_assert(std::is_trivially_copyable_v<Tile2DSlotReply>);

    template <>
    struct CommandTraits<AddTile2DPayload>
    {
        using Reply = Tile2DSlotReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = reply_type_id_of_v<Tile2DSlotReply>;
    };

    struct RemoveTile2DPayload
    {
        RenderSceneId        scene{};
        Tile2DInstanceHandle handle{};
    };
    static_assert(std::is_trivially_copyable_v<RemoveTile2DPayload>);

    /// Tilemaps are FEW (a handful per scene) and mostly static — plain Stream
    /// ops per moved map, like fields.
    struct UpdateTile2DTransformPayload
    {
        RenderSceneId        scene{};
        Tile2DInstanceHandle handle{};
        float                m[6]{};
    };
    static_assert(std::is_trivially_copyable_v<UpdateTile2DTransformPayload>);

    struct UpdateTile2DKeyPayload
    {
        RenderSceneId        scene{};
        Tile2DInstanceHandle handle{};
        float                priority{0.f};
        std::uint32_t        visible{1};
    };
    static_assert(std::is_trivially_copyable_v<UpdateTile2DKeyPayload>);

    // ── v2 typed op declarations (order == Canvas2DOperationIds order) ─────────
    struct AddSprite2DOp
    {
        using Payload = AddSprite2DPayload;
        static constexpr EOpKind kind = EOpKind::Stream;   // CommandOp + replyToCurrent
        static constexpr const char* name = "AddSprite2D";
    };
    struct RemoveSprite2DOp
    {
        using Payload = RemoveSprite2DPayload;
        static constexpr EOpKind kind = EOpKind::Stream;
        static constexpr const char* name = "RemoveSprite2D";
    };
    struct Sprite2DTransformBatchOp
    {
        using Payload = Sprite2DTransformEntry;
        static constexpr EOpKind kind = EOpKind::Bulk;
        static constexpr const char* name = "Sprite2DTransformBatch";
    };
    struct UpdateSprite2DVisualOp
    {
        using Payload = UpdateSprite2DVisualPayload;
        static constexpr EOpKind kind = EOpKind::Stream;
        static constexpr const char* name = "UpdateSprite2DVisual";
    };
    struct UpdateSprite2DKeyOp
    {
        using Payload = UpdateSprite2DKeyPayload;
        static constexpr EOpKind kind = EOpKind::Stream;
        static constexpr const char* name = "UpdateSprite2DKey";
    };
    struct SetCanvas2DEnabledOp
    {
        using Payload = SetCanvas2DEnabledPayload;
        static constexpr EOpKind kind = EOpKind::Stream;
        static constexpr const char* name = "SetCanvas2DEnabled";
    };
    struct AddPixelField2DOp
    {
        using Payload = AddPixelField2DPayload;
        static constexpr EOpKind kind = EOpKind::Stream;   // CommandOp + replyToCurrent
        static constexpr const char* name = "AddPixelField2D";
    };
    struct RemovePixelField2DOp
    {
        using Payload = RemovePixelField2DPayload;
        static constexpr EOpKind kind = EOpKind::Stream;
        static constexpr const char* name = "RemovePixelField2D";
    };
    struct UpdatePixelField2DTransformOp
    {
        using Payload = UpdatePixelField2DTransformPayload;
        static constexpr EOpKind kind = EOpKind::Stream;
        static constexpr const char* name = "UpdatePixelField2DTransform";
    };
    struct UpdatePixelField2DKeyOp
    {
        using Payload = UpdatePixelField2DKeyPayload;
        static constexpr EOpKind kind = EOpKind::Stream;
        static constexpr const char* name = "UpdatePixelField2DKey";
    };
    struct AddTile2DOp
    {
        using Payload = AddTile2DPayload;
        static constexpr EOpKind kind = EOpKind::Stream;   // CommandOp + replyToCurrent
        static constexpr const char* name = "AddTile2D";
    };
    struct RemoveTile2DOp
    {
        using Payload = RemoveTile2DPayload;
        static constexpr EOpKind kind = EOpKind::Stream;
        static constexpr const char* name = "RemoveTile2D";
    };
    struct UpdateTile2DTransformOp
    {
        using Payload = UpdateTile2DTransformPayload;
        static constexpr EOpKind kind = EOpKind::Stream;
        static constexpr const char* name = "UpdateTile2DTransform";
    };
    struct UpdateTile2DKeyOp
    {
        using Payload = UpdateTile2DKeyPayload;
        static constexpr EOpKind kind = EOpKind::Stream;
        static constexpr const char* name = "UpdateTile2DKey";
    };

    /// Op ids returned to the client after RegisterFeatureType (forward-declarable,
    /// so gameplay bridges stay render-light — mirrors MeshStackOperationIds).
    struct Canvas2DOperationIds
        : FeatureOpIds<AddSprite2DOp, RemoveSprite2DOp, Sprite2DTransformBatchOp,
                       UpdateSprite2DVisualOp, UpdateSprite2DKeyOp, SetCanvas2DEnabledOp,
                       AddPixelField2DOp, RemovePixelField2DOp,
                       UpdatePixelField2DTransformOp, UpdatePixelField2DKeyOp,
                       AddTile2DOp, RemoveTile2DOp,
                       UpdateTile2DTransformOp, UpdateTile2DKeyOp>
    {
        using Ids = FeatureOpIds<AddSprite2DOp, RemoveSprite2DOp, Sprite2DTransformBatchOp,
                                 UpdateSprite2DVisualOp, UpdateSprite2DKeyOp, SetCanvas2DEnabledOp,
                                 AddPixelField2DOp, RemovePixelField2DOp,
                                 UpdatePixelField2DTransformOp, UpdatePixelField2DKeyOp,
                                 AddTile2DOp, RemoveTile2DOp,
                                 UpdateTile2DTransformOp, UpdateTile2DKeyOp>;
        Canvas2DOperationIds() = default;
        Canvas2DOperationIds(const Ids& base) noexcept : Ids(base) {}
        [[nodiscard]] static Canvas2DOperationIds fromOps(const TypeId* ops, std::uint32_t count) noexcept
        {
            return Canvas2DOperationIds{Ids::fromOps(ops, count)};
        }
    };

    /// Canvas2DFeature factory (create + register_ops_fn allocating the 6 ops above).
    extern LUX_FUNCTION_PUBLIC const FeatureFactory kCanvas2DFeatureFactory;

    /// Client proxy — issues v2 instance commands against a scene's Canvas2DFeature.
    /// All methods no-op when the feature exposes no ops (unregistered scene → a
    /// pure-3D host pays nothing). Wired to the retained Sprite2DBridge in C2D-R3.
    class LUX_FUNCTION_PUBLIC Canvas2DProxy
    {
    public:
        Canvas2DProxy(RenderSession& session, Canvas2DOperationIds ops) noexcept
            : session_(&session), ops_(ops) {}

        /// Create one GPU-resident sprite; replies with {handle, status} (G-05).
        [[nodiscard]] RenderRequest<Sprite2DSlotReply> addSprite(
            RenderSceneId scene, const Sprite2DInstanceData& data,
            float priority, bool visible = true);

        void removeSprite(RenderSceneId scene, Sprite2DHandle handle);

        /// Per-frame dirty transform deltas. Entries already carry their scene
        /// (G-04); the scene-stamping overload covers the common one-scene batch.
        void updateTransforms(std::span<const Sprite2DTransformEntry> entries);
        void updateTransforms(RenderSceneId scene, std::span<Sprite2DTransformEntry> entries);
        void updateTransform(RenderSceneId scene, Sprite2DHandle handle, const float m[6]);

        void updateVisual(RenderSceneId scene, Sprite2DHandle handle,
                          const float uv[4], std::uint32_t tint, std::uint32_t texture_bindless);
        void updateKey(RenderSceneId scene, Sprite2DHandle handle, float priority, bool visible);
        void setEnabled(RenderSceneId scene, bool enabled);

        // ── PixelField kind (F2-09) ──
        [[nodiscard]] RenderRequest<PixelFieldSlotReply> addPixelField(
            RenderSceneId scene, const PixelField2DInstanceData& data,
            float priority, bool visible = true);
        void removePixelField(RenderSceneId scene, PixelFieldInstanceHandle handle);
        void updatePixelFieldTransform(RenderSceneId scene, PixelFieldInstanceHandle handle,
                                       const float m[6]);
        void updatePixelFieldKey(RenderSceneId scene, PixelFieldInstanceHandle handle,
                                 float priority, bool visible);

        // ── Tile kind (A2-02) ──
        [[nodiscard]] RenderRequest<Tile2DSlotReply> addTilemap(
            RenderSceneId scene, const Tile2DInstanceData& data,
            float priority, bool visible = true);
        void removeTilemap(RenderSceneId scene, Tile2DInstanceHandle handle);
        void updateTilemapTransform(RenderSceneId scene, Tile2DInstanceHandle handle,
                                    const float m[6]);
        void updateTilemapKey(RenderSceneId scene, Tile2DInstanceHandle handle,
                              float priority, bool visible);

        [[nodiscard]] bool valid() const noexcept { return ops_.valid(); }

    private:
        RenderSession*       session_;
        Canvas2DOperationIds ops_;
    };

} // namespace lux::render
