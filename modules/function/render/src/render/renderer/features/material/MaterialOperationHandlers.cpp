// ============================================================================
//  MaterialOperationHandlers.cpp — StandardMaterialFeature factory + the
//  feature-scoped material commands. The upload / modify / destroy handlers live
//  HERE (a feature), not in the core RenderServer dispatcher, and are registered
//  with DYNAMIC TypeIds via register_ops_fn (the grid / light pattern). The core
//  protocol no longer names material ops.
//
//  The global material stack (MaterialResources + ShadingModelRegistry), the
//  graph-material submit/modify/remove assembly, and scene-graph invalidation all live
//  HERE now (Stage C) — feature-internal; the core RenderServer no longer hosts any
//  material body. The assembly reaches the server's Vulkan stack through
//  GeneralRenderServer::Impl (RenderServerImpl.hpp, project-visible), casting the
//  dispatcher's void* user_state.
// ============================================================================

#include <lux/engine/render/comm/server/RenderServer.hpp>         // Dispatcher, Ctx, replyToCurrent, FeatureFactory, resolveExternalData/resolveBlob
#include <lux/engine/render/comm/server/RenderServerImpl.hpp>     // GeneralRenderServer::Impl (assembly host)
#include <lux/engine/render/comm/server/FeatureOpRegistrar.hpp>   // typed-op register/unregister
#include <lux/engine/render/renderer/features/FeatureOpSend.hpp>  // send / sendWithReply / sendBlob
#include <lux/engine/render/renderer/features/material/MaterialOperation.hpp>
#include <lux/engine/render/renderer/features/material/StandardMaterialFeature.hpp>
#include <lux/engine/render/comm/client/RenderSession.hpp>        // MaterialProxy: builder / pushBorrowedBytes / RenderRequest
#include <lux/engine/render/resources/material/GraphMaterialData.hpp>
#include <lux/engine/render/resources/material/MaterialResources.hpp> // MaterialResources, MaterialHandle, submitGraph
#include <lux/engine/render/pipeline/ShadingModelRegistry.hpp>    // ensureGlobalMaterialResources emplaces it
#include <lux/engine/render/pipeline/GeneralDescriptorSetLayout.hpp> // getMaterialSetLayout()
#include <lux/engine/description/MaterialEnums.hpp>               // rdesc::EAlphaMode
#include <lux/engine/render/scene/RenderScene.hpp>

#include <iostream>   // std::cerr (global-resource init failure)
#include <cstdint>
#include <span>

namespace lux::render
{
    using Dispatcher = GeneralRenderServer::Dispatcher;
    using Ctx        = Dispatcher::Ctx;

    // ── GLOBAL material stack (ShadingModelRegistry + MaterialResources) ───────
    // External linkage: StandardMaterialFeature::initAndAttachTo also triggers this (it
    // forward-declares it), so it is NOT in the anonymous namespace. Lazy + idempotent;
    // registry-first because MaterialResources::init requires ShadingModelRegistry. A
    // server whose scenes never add StandardMaterial / never upload a material (pure 2D /
    // headless) never allocates the material stack.
    void ensureGlobalMaterialResources(RenderContext& ctx)
    {
        auto& greg = ctx.globalRegistry();
        if (!greg.find<ShadingModelRegistry>())
            greg.emplace<ShadingModelRegistry>();
        if (greg.find<MaterialResources>())
            return;

        MaterialResources::InitInfo info{};
        info.ssbo_config = SSBOInitConfig{
            .device_context         = &ctx.deviceContext(),
            .initial_dense_capacity = 512,
            .slices                 = ctx.framesInFlight(),
        };
        info.descriptor_pool = ctx.resourceContext().descriptorPool().handle();
        info.set_layout      = ctx.descriptorLayouts().getMaterialSetLayout();
        info.registry        = greg.find<ShadingModelRegistry>();
        greg.emplace<MaterialResources>();
        auto* mat = greg.find<MaterialResources>();
        if (mat->init(info))
            mat->setDeferredQueue(&ctx.deferredDestroyQueue());
        else
            // Uninitialized-on-failure survives in the registry (no erase API; no retry —
            // see ensureGlobalMeshResources). The material server bodies guard every read
            // via isInitialized(), so the dead instance is never operated on.
            std::cerr << "[StandardMaterial] global MaterialResources init failed; materials will not render.\n";
    }

    namespace
    {
        // ── Material server-side assembly (moved out of RenderServer.cpp, Stage C) ──
        // Internal to this TU: only the handlers below call them. They reach the server's
        // Vulkan stack through GeneralRenderServer::Impl (the void* user_state).

        RMaterialHandle serverUploadGraphMaterial(
            void* user_state, const GraphMaterialData& data,
            ShaderHandle gbuffer_shader, ShaderHandle forward_shader,
            std::uint64_t shader_key, std::uint32_t alpha_mode, bool double_sided)
        {
            auto& im = *static_cast<GeneralRenderServer::Impl*>(user_state);
            // The lazy material stack is normally built at StandardMaterial attach, but
            // never rely on dispatch order — self-heal here. Fail closed if it never
            // initialized (the uninitialized object survives in the registry → guard via
            // isInitialized()).
            ensureGlobalMaterialResources(*im.render_ctx_);
            auto* mat_res = im.render_ctx_->globalRegistry().find<MaterialResources>();
            if (!mat_res || !mat_res->isInitialized())
                return RMaterialHandle{};

            auto result = mat_res->submitGraph(
                data, gbuffer_shader, forward_shader, shader_key,
                static_cast<lux::rdesc::EAlphaMode>(alpha_mode), double_sided);
            if (!result)
                return RMaterialHandle{};

            // Any new material invalidates compiled scene graphs (per-material PSO sets).
            if (im.renderer_)
                im.renderer_->forEachScene([](RenderScene& s) { s.invalidateGraph(); });

            auto h = result.value();
            return RMaterialHandle{h.index, h.gen};
        }

        void serverModifyGraphMaterial(void* user_state, RMaterialHandle handle, const GraphMaterialData& data)
        {
            auto& im = *static_cast<GeneralRenderServer::Impl*>(user_state);
            ensureGlobalMaterialResources(*im.render_ctx_);
            if (auto* mat = im.render_ctx_->globalRegistry().find<MaterialResources>();
                mat && mat->isInitialized())
                mat->modifyGraph(MaterialHandle{handle.index, handle.gen}, data);
        }

        void serverDestroyMaterial(void* user_state, RMaterialHandle handle)
        {
            auto& im = *static_cast<GeneralRenderServer::Impl*>(user_state);
            ensureGlobalMaterialResources(*im.render_ctx_);
            auto* mat = im.render_ctx_->globalRegistry().find<MaterialResources>();
            if (!mat || !mat->isInitialized())
                return;
            mat->remove(MaterialHandle{handle.index, handle.gen});
            if (im.renderer_)
                im.renderer_->forEachScene([](RenderScene& s) { s.invalidateGraph(); });
        }
        void handleUploadGraphMaterial(Ctx& ctx, const UploadGraphMaterialPayload& p)
        {
            auto desc_bytes = resolveExternalData(ctx.program, p.graph_desc);
            const auto* data = reinterpret_cast<const GraphMaterialData*>(desc_bytes.data());
            const RMaterialHandle h = serverUploadGraphMaterial(
                ctx.user_state, *data, p.graph_gbuffer_shader, p.graph_forward_shader,
                p.shader_key, p.alpha_mode, p.double_sided != 0u);
            replyToCurrent<UploadGraphMaterialPayload>(ctx,
                MaterialUploadedReply{h, h.is_null() ? 1u : 0u});
        }

        void handleModifyGraphMaterial(Ctx& ctx, const ModifyGraphMaterialPayload& p)
        {
            auto desc_bytes = resolveBlob(ctx.program, p.graph_desc);
            const auto* data = reinterpret_cast<const GraphMaterialData*>(desc_bytes.data());
            serverModifyGraphMaterial(ctx.user_state, p.handle, *data);
        }

        void handleDestroyMaterial(Ctx& ctx, const DestroyMaterialPayload& p)
        {
            serverDestroyMaterial(ctx.user_state, p.handle);
        }
    } // namespace

    // ── Uniform factory interface ────────────────────────────────────────────
    static FeatureHandle standardMaterialCreateFn(void* scene_ptr, const void* /*param*/, size_t /*sz*/)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);
        StandardMaterialFeature::Config cfg{};
        return sc->addFeature<StandardMaterialFeature>(cfg);
    }

    using MaterialOps = FeatureOpRegistrar<
        ServerOp<UploadGraphMaterialOp, &handleUploadGraphMaterial>,
        ServerOp<ModifyGraphMaterialOp, &handleModifyGraphMaterial>,
        ServerOp<DestroyMaterialOp,     &handleDestroyMaterial>>;

    const FeatureFactory kStandardMaterialFeatureFactory{
        &standardMaterialCreateFn,
        &MaterialOps::registerAll,
        &MaterialOps::unregisterAll,
        "StandardMaterial",
    };

    // ── Client-side proxy (typed-op send-routing) ─────────────────────────────
    RenderRequest<MaterialUploadedReply> MaterialProxy::uploadGraphMaterial(const GraphMaterialData& data)
    {
        UploadGraphMaterialPayload ugp{};
        // BORROW the data until submitFrame (NOT a blob copy) — matches the retired
        // RenderSession::uploadGraphMaterial.
        ugp.graph_desc = session_->builder().pushBorrowedBytes(
            reinterpret_cast<const std::byte*>(&data),
            static_cast<std::uint32_t>(sizeof(GraphMaterialData)));
        return sendWithReply<UploadGraphMaterialOp>(*session_, ops_, ugp);
    }

    RenderRequest<MaterialUploadedReply> MaterialProxy::uploadGraphMaterial(
        const GraphMaterialData& data,
        ShaderHandle  gbuffer_shader,
        ShaderHandle  forward_shader,
        std::uint32_t alpha_mode,
        bool          double_sided)
    {
        UploadGraphMaterialPayload ugp{};
        ugp.graph_desc = session_->builder().pushBorrowedBytes(
            reinterpret_cast<const std::byte*>(&data),
            static_cast<std::uint32_t>(sizeof(GraphMaterialData)));
        ugp.graph_gbuffer_shader = gbuffer_shader;
        ugp.graph_forward_shader = forward_shader;
        // shader_key: a stable hash of the two handles (non-zero so submitGraph routes
        // this material to its OWN bucket). Identical shaders -> identical key -> shared
        // bucket. (Render-state folds into the bucket key on the server, not here.)
        ugp.shader_key =
            (static_cast<std::uint64_t>(gbuffer_shader.index) << 40)
          ^ (static_cast<std::uint64_t>(gbuffer_shader.gen)   << 32)
          ^ (static_cast<std::uint64_t>(forward_shader.index) << 8)
          ^  static_cast<std::uint64_t>(forward_shader.gen);
        if (ugp.shader_key == 0) ugp.shader_key = 1; // never alias the legacy 0
        ugp.alpha_mode   = alpha_mode;
        ugp.double_sided = double_sided ? 1u : 0u;
        return sendWithReply<UploadGraphMaterialOp>(*session_, ops_, ugp);
    }

    void MaterialProxy::modifyGraphMaterial(RMaterialHandle handle, const GraphMaterialData& data)
    {
        ModifyGraphMaterialPayload p{};
        p.handle = handle;
        // COPY into the ring-slot blob (not a borrow): per-frame fire-and-forget; the
        // caller's animated GraphMaterialData is transient. sendBlob writes the BlobRef
        // into p.graph_desc (Op::blob_field) then routes by reply-ness (none → send).
        sendBlob<ModifyGraphMaterialOp>(*session_, ops_, p,
            std::span<const std::byte>{ reinterpret_cast<const std::byte*>(&data), sizeof(GraphMaterialData) },
            alignof(GraphMaterialData));
    }

    void MaterialProxy::destroyMaterial(RMaterialHandle handle)
    {
        DestroyMaterialPayload p{};
        p.handle = handle;
        send<DestroyMaterialOp>(*session_, ops_, p);
    }

} // namespace lux::render
