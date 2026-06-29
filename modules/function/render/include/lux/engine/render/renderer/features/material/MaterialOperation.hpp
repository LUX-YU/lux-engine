#pragma once
// ============================================================================
//  MaterialOperation.hpp — StandardMaterialFeature factory + feature-scoped
//  material ops (uploadGraphMaterial / modifyGraphMaterial / destroyMaterial).
//
//  The global material stack (ShadingModelRegistry + MaterialResources) is a
//  FEATURE domain (Stage D-2). The commands that create / mutate / destroy
//  materials are dispatched by dynamically-allocated TypeIds (register_ops_fn —
//  the grid / light pattern) and sent via a feature-scoped MaterialProxy. The
//  core RenderProtocol no longer names material ops. A 2D / unlit / headless
//  scene that omits StandardMaterial carries none of this vocabulary.
//
//  uploadGraphMaterial REPLIES with the new RMaterialHandle. Its payload BORROWS
//  the GraphMaterialData (ExternalDataRef, valid until submitFrame); modify COPIES
//  it (BlobRef, per-frame). The proxy therefore routes the two differently — upload
//  fills the borrow then sendWithReply<Resource>, modify uses sendBlob.
// ============================================================================

#include <lux/engine/render/comm/RenderCommTypes.hpp>       // TypeId, opcodes, CommandTraits, ExternalDataRef, BlobRef
#include <lux/engine/render/core/RenderResourceHandle.hpp>  // RMaterialHandle
#include <lux/engine/render/core/ResourceHandle.hpp>        // ShaderHandle
#include <lux/engine/render/resources/ops/ResourceOperationCommon.hpp>  // DestroyResourcePayload
#include <lux/engine/render/renderer/features/FeatureOps.hpp>
#include <lux/engine/render/resources/material/GraphMaterialData.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <span>
#include <type_traits>

namespace lux::render
{
    struct FeatureFactory;
    class RenderSession;
    template <typename T> class RenderRequest;

    // =========================================================================
    //  Material command payloads + reply (moved out of the core RenderProtocol.hpp —
    //  the core no longer names them; this feature header is their SSOT now).
    // =========================================================================
    using DestroyMaterialPayload = DestroyResourcePayload<RMaterialHandle>;
    static_assert(std::is_trivially_copyable_v<DestroyMaterialPayload>);

    /// Node-graph (Graph family) material upload. The descriptor is a flat
    /// GraphMaterialData blob BORROWED via ExternalDataRef (valid until submitFrame).
    struct UploadGraphMaterialPayload
    {
        ExternalDataRef graph_desc{};            // points to const GraphMaterialData
        ShaderHandle    graph_gbuffer_shader{};  // R1: per-material baked frags → own bucket/PSO
        ShaderHandle    graph_forward_shader{};
        uint64_t        shader_key{ 0 };
        uint32_t        alpha_mode{ 0 };   // rdesc::EAlphaMode: 0=Opaque,1=Mask,2=Blend
        uint8_t         double_sided{ 0 };
    };
    static_assert(std::is_trivially_copyable_v<UploadGraphMaterialPayload>);

    /// In-place per-frame update of an existing Graph material (fire-and-forget).
    /// graph_desc is an in-payload BLOB (copied at push time), NOT a borrow.
    struct ModifyGraphMaterialPayload
    {
        RMaterialHandle handle{};
        BlobRef         graph_desc{}; // GraphMaterialData copied into the payload blob
    };
    static_assert(std::is_trivially_copyable_v<ModifyGraphMaterialPayload>);

    /// Reply for uploadGraphMaterial — the new material handle + status.
    struct MaterialUploadedReply
    {
        RMaterialHandle handle{};
        uint32_t        status{0};
    };
    static_assert(std::is_trivially_copyable_v<MaterialUploadedReply>);

    template <>
    struct CommandTraits<UploadGraphMaterialPayload>
    {
        using Reply = MaterialUploadedReply;
        static constexpr bool has_reply = true;
        // Derived from the Reply TYPE (no hand-picked core ReplyMaterialUploaded tag).
        static constexpr TypeId reply_type_id = reply_type_id_of_v<MaterialUploadedReply>;
    };

    // =========================================================================
    //  Feature-scoped operation name constants (ids allocated dynamically).
    // =========================================================================
    // =========================================================================
    //  Typed op declarations.
    // =========================================================================
    struct UploadGraphMaterialOp
    {
        using Payload = UploadGraphMaterialPayload;
        static constexpr EOpKind kind = EOpKind::Resource;   // replies RMaterialHandle (pushResource)
        static constexpr const char* name = "UploadGraphMaterial";
    };
    struct ModifyGraphMaterialOp
    {
        using Payload = ModifyGraphMaterialPayload;
        static constexpr EOpKind kind = EOpKind::Blob;       // GraphMaterialData copied into a BlobRef
        static constexpr OpCode  opcode = opcodes::ResourceOp;   // rides the resource ring (no reply)
        static constexpr const char* name = "ModifyGraphMaterial";
        static constexpr auto blob_field = &ModifyGraphMaterialPayload::graph_desc;
    };
    struct DestroyMaterialOp
    {
        using Payload = DestroyMaterialPayload;
        static constexpr EOpKind kind = EOpKind::Stream;
        static constexpr OpCode  opcode = opcodes::ResourceOp;   // resource lifecycle (no reply)
        static constexpr const char* name = "DestroyMaterial";
    };

    /// Operation IDs returned to the client after RegisterFeatureType. A real
    /// (forward-declarable) struct rather than a `using` alias, because gameplay
    /// headers forward-declare it to stay render-light.
    struct MaterialOperationIds
        : FeatureOpIds<UploadGraphMaterialOp, ModifyGraphMaterialOp, DestroyMaterialOp>
    {
        using Ids = FeatureOpIds<UploadGraphMaterialOp, ModifyGraphMaterialOp, DestroyMaterialOp>;
        MaterialOperationIds() = default;
        MaterialOperationIds(const Ids& base) noexcept : Ids(base) {}
        [[nodiscard]] static MaterialOperationIds fromOps(const TypeId* ops, std::uint32_t count) noexcept
        {
            return MaterialOperationIds{Ids::fromOps(ops, count)};
        }
    };

    /// StandardMaterialFeature factory (create + register_ops_fn allocating the 3
    /// ops above). Adding it IS the opt-in to standard materials. MUST be added
    /// BEFORE every material consumer (DeferredGBuffer / ForwardMesh / Highlight)
    /// AND before StandardMeshStack (addMeshInstance reads the material slot).
    extern LUX_FUNCTION_PUBLIC const FeatureFactory kStandardMaterialFeatureFactory;

    // =========================================================================
    //  Client-side proxy — MaterialProxy (sends material CRUD via dynamic ids).
    //  Replaces the retired RenderSession::uploadGraphMaterial / modifyGraphMaterial
    //  / destroyMaterial.
    // =========================================================================
    class LUX_FUNCTION_PUBLIC MaterialProxy
    {
    public:
        MaterialProxy(RenderSession& session, MaterialOperationIds ops) noexcept
            : session_(&session), ops_(ops) {}

        /// Upload a node-graph material (Graph family); replies with its handle.
        /// The data is BORROWED until submitFrame — keep it alive that long.
        [[nodiscard]] RenderRequest<MaterialUploadedReply> uploadGraphMaterial(const GraphMaterialData& data);
        /// R1 overload: per-material baked frag shaders → own variant bucket / PSO.
        [[nodiscard]] RenderRequest<MaterialUploadedReply> uploadGraphMaterial(
            const GraphMaterialData& data,
            ShaderHandle  gbuffer_shader,
            ShaderHandle  forward_shader,
            std::uint32_t alpha_mode = 0,
            bool          double_sided = false);

        /// In-place per-frame update of an existing Graph material (fire-and-forget;
        /// the data is COPIED into the command, so it may be transient).
        void modifyGraphMaterial(RMaterialHandle handle, const GraphMaterialData& data);

        /// Destroy a material (fire-and-forget).
        void destroyMaterial(RMaterialHandle handle);

        [[nodiscard]] bool valid() const noexcept { return ops_.valid(); }

    private:
        RenderSession*       session_;
        MaterialOperationIds ops_;
    };

} // namespace lux::render
