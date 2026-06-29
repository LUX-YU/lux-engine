#pragma once
// ============================================================================
//  SkinningOperation.hpp — SkinningFeature factory + feature-scoped commands.
//
//  GPU skinning (bone palettes) is a FEATURE domain, not a core protocol op.
//  The per-instance bone-palette upload + the batched variant live here, are
//  dispatched by dynamically-allocated TypeIds (registered via the feature's
//  register_ops_fn — the grid pattern), and are sent by a feature-scoped
//  SkinningProxy. The core RenderProtocol.hpp no longer names skinning.
//  (See .internal/render-architecture-decoupling-design-2026-06-19.md 契约 C5.)
// ============================================================================

#include <lux/engine/render/comm/RenderCommTypes.hpp>          // BlobRef, TypeId, opcodes
#include <lux/engine/render/renderer/features/FeatureOps.hpp>  // EOpKind / FeatureOpIds
#include <lux/engine/render/core/RenderResourceHandle.hpp>     // RMeshHandle
#include <lux/engine/render/core/RenderObjectTypes.hpp>        // RenderObjectHandle
#include <lux/engine/render/core/RenderSceneId.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <span>
#include <type_traits>

namespace lux::render
{
    struct FeatureFactory;
    class RenderSession;

    // ---- Per-instance bone palette upload (GPU skinning) ----
    // Fire-and-forget CommandOp carrying a variable-length bone-matrix blob.
    // The handler resolves the instance's input vertex range from `mesh`, uploads
    // the palette + records a skinning dispatch, and patches the instance's
    // InstanceProperty.vertex_pool_id/base to the skinned output.
    struct UploadBonePalettePayload
    {
        RenderSceneId      scene_id{};
        RenderObjectHandle object{};       ///< instance to skin (patched in place)
        RMeshHandle        mesh{};         ///< source mesh → input vertex range
        uint32_t           bone_count{0};
        // In-payload BLOB (copied into the ring-slot blob at push time), NOT a
        // borrowed pointer: fire-and-forget per-frame command; the consumer
        // advances the SPSC ring before the handler reads, so a borrow would race
        // the game thread clearing/reallocating its bone buffer next tick.
        BlobRef            bones{};        ///< bone_count × BoneMatrixGpu (mat4, 64B)
    };
    static_assert(std::is_trivially_copyable_v<UploadBonePalettePayload>);

    // ---- Batched per-instance bone palettes (one command per frame) ----
    // `entries` is entry_count × BoneBatchEntry; `bones` is the concatenated
    // BoneMatrixGpu blob, each entry slicing [bone_offset, bone_offset+bone_count).
    struct BoneBatchEntry
    {
        RenderObjectHandle object{};       ///< instance to skin (patched in place)
        RMeshHandle        mesh{};         ///< source mesh → input vertex range
        uint32_t           bone_offset{0}; ///< first bone in the shared blob (mat4 units)
        uint32_t           bone_count{0};
    };
    static_assert(std::is_trivially_copyable_v<BoneBatchEntry>);

    struct UploadBoneBatchPayload
    {
        RenderSceneId   scene_id{};
        uint32_t        entry_count{0};
        BlobRef         entries{};   ///< entry_count × BoneBatchEntry
        BlobRef         bones{};     ///< concatenated BoneMatrixGpu (mat4, 64 B each)
    };
    static_assert(std::is_trivially_copyable_v<UploadBoneBatchPayload>);

    // =========================================================================
    //  Typed op declarations. Both are fire-and-forget (CommandOp, no reply).
    //  Palette carries ONE blob (bones) → Blob kind; Batch carries TWO blobs
    //  (entries + bones) → plain Stream, the proxy fills both BlobRefs by hand
    //  (the single-blob sendBlob sugar can't express two).
    // =========================================================================
    struct UploadBonePaletteOp
    {
        using Payload = UploadBonePalettePayload;
        static constexpr EOpKind kind = EOpKind::Blob;
        static constexpr const char* name = "UploadBonePalette";
        static constexpr auto blob_field = &UploadBonePalettePayload::bones;
    };
    struct UploadBoneBatchOp
    {
        using Payload = UploadBoneBatchPayload;
        static constexpr EOpKind kind = EOpKind::Stream;   // two blobs → manual fill
        static constexpr const char* name = "UploadBoneBatch";
    };

    // =========================================================================
    //  Operation IDs returned to the client after RegisterFeatureType. A real
    //  (forward-declarable) struct rather than a `using` alias, because gameplay
    //  headers forward-declare it to stay render-light.
    // =========================================================================
    struct SkinningOperationIds
        : FeatureOpIds<UploadBonePaletteOp, UploadBoneBatchOp>
    {
        using Ids = FeatureOpIds<UploadBonePaletteOp, UploadBoneBatchOp>;
        SkinningOperationIds() = default;
        SkinningOperationIds(const Ids& base) noexcept : Ids(base) {}
        [[nodiscard]] static SkinningOperationIds fromOps(const TypeId* ops, std::uint32_t count) noexcept
        {
            return SkinningOperationIds{Ids::fromOps(ops, count)};
        }
    };

    /// SkinningFeature factory (create + register_ops_fn allocating the 2 ops above).
    extern LUX_FUNCTION_PUBLIC const FeatureFactory kSkinningFeatureFactory;

    // =========================================================================
    //  Client-side proxy — SkinningProxy (sends bone uploads via the dynamic ids)
    // =========================================================================
    class LUX_FUNCTION_PUBLIC SkinningProxy
    {
    public:
        SkinningProxy(RenderSession& session, SkinningOperationIds ops) noexcept
            : session_(&session), ops_(ops) {}

        /// Single-instance bone palette upload (fire-and-forget).
        void uploadBonePalette(RenderSceneId scene_id, RenderObjectHandle object,
                               RMeshHandle mesh, const void* bone_matrices,
                               uint32_t bone_count);

        /// Batched bone palettes — one command carrying N instances + a shared
        /// bone-matrix blob (the per-frame path used by the skeletal-mesh adapter).
        void uploadBoneBatch(RenderSceneId scene_id, std::span<const BoneBatchEntry> entries,
                             const void* bone_matrices, uint32_t bone_total_count);

        [[nodiscard]] bool valid() const noexcept { return ops_.valid(); }

    private:
        RenderSession*       session_;
        SkinningOperationIds ops_;
    };

} // namespace lux::render
