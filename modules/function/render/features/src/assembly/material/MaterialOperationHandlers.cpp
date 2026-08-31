// ============================================================================
//  MaterialOperationHandlers.cpp — StandardMaterialFeature factory + the
//  feature-scoped material commands. The upload / modify / destroy handlers live
//  HERE (a feature), not in the core RenderServer dispatcher, and are registered
//  with DYNAMIC TypeIds via register_ops_fn (the grid / light pattern). The core
//  protocol no longer names material ops.
//
//  The global MaterialResources stack, the
//  graph-material submit/modify/remove assembly, and scene-graph invalidation all live
//  HERE now (Stage C) — feature-internal; the core RenderServer no longer hosts any
//  material body. The assembly reaches the server's Vulkan stack through
//  窄 shim(lookupScene / lookupRenderContext / forEachSceneOnServer,定义在
//  RenderServer.cpp),不再 include 服务端的私有 Impl 头。
// ============================================================================

#include <lux/engine/render/comm/server/RenderServer.hpp>
// Dispatcher, Ctx, replyToCurrent, FeatureFactory, resolveExternalData/resolveBlob
#include <lux/engine/render/comm/server/FeatureOpRegistrar.hpp> // typed-op register/unregister
#include <lux/engine/function/render/client/genops/MaterialOperation.ops.hpp>
#include <lux/engine/render/renderer/features/material/StandardMaterialFeature.hpp>
#include <lux/engine/function/render/client/resources/material/GraphMaterialData.hpp>
#include <lux/engine/render/resources/material/MaterialResources.hpp> // MaterialResources, MaterialHandle, submitGraph
#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp> // getMaterialSetLayout()
#include <lux/engine/description/MaterialEnums.hpp>                      // rdesc::EAlphaMode
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp> // lookupRenderContext 的返回类型

#include <cstdint>

namespace lux::render
{
    using Dispatcher = GeneralRenderServer::Dispatcher;
    using Ctx = Dispatcher::Ctx;

    //(ensureGlobalMaterialResources 已下沉到 L3 的
    // src/render/resources/material/MaterialResources.cpp —— 理由同
    // ensureGlobalMeshResources。声明见 resources/material/MaterialResources.hpp。)

    // 服务端的窄 shim(定义在 RenderServer.cpp)—— 与 lookupScene 同一条约定。
    RenderContext* lookupRenderContext(void* user_state);
    void forEachSceneOnServer(void* user_state, void (*fn)(RenderScene&));

    namespace
    {
        // ── Material server-side assembly (moved out of RenderServer.cpp, Stage C) ──
        // Internal to this TU: only the handlers below call them. They reach the server's
        // 服务端的东西经窄 shim 取(lookupScene / lookupRenderContext)。

        RMaterialHandle serverUploadGraphMaterial(
            void* user_state,
            asset::AssetId asset_id,
            const GraphMaterialData& data,
            ShaderHandle gbuffer_shader,
            ShaderHandle forward_shader,
            std::uint64_t shader_key,
            std::uint32_t alpha_mode,
            bool double_sided
        )
        {
            auto* rctx = lookupRenderContext(user_state);
            if (!rctx)
                return RMaterialHandle{};
            // The lazy material stack is normally built at StandardMaterial attach, but
            // never rely on dispatch order — self-heal here. Fail closed if it never
            // initialized (the uninitialized object survives in the registry → guard via
            // isInitialized()).
            if (!ensureGlobalMaterialResources(*rctx))
                return RMaterialHandle{};
            auto* mat_res = rctx->globalRegistry().find<MaterialResources>();
            if (!mat_res || !mat_res->isInitialized())
                return RMaterialHandle{};

            if (const auto existing = mat_res->findAsset(asset_id))
            {
                return RMaterialHandle{existing->index, existing->gen};
            }

            auto result = mat_res->submitGraph(
                asset_id,
                data,
                gbuffer_shader,
                forward_shader,
                shader_key,
                static_cast<lux::rdesc::EAlphaMode>(alpha_mode),
                double_sided
            );
            if (!result)
                return RMaterialHandle{};

            // Any new material invalidates compiled scene graphs (per-material PSO sets).
            forEachSceneOnServer(user_state, [](RenderScene& scene) {
                scene.invalidateGraph(EGraphInvalidationReason::MATERIAL_LAYOUT);
            }
            );

            auto h = result.value();
            return RMaterialHandle{h.index, h.gen};
        }

        void serverModifyGraphMaterial(void* user_state, RMaterialHandle handle, const GraphMaterialData& data)
        {
            auto* rctx = lookupRenderContext(user_state);
            if (!rctx)
                return;
            if (!ensureGlobalMaterialResources(*rctx))
                return;
            if (auto* mat = rctx->globalRegistry().find<MaterialResources>(); mat && mat->isInitialized())
                mat->modifyGraph(MaterialHandle{handle.index, handle.gen}, data);
        }

        void serverDestroyMaterial(void* user_state, RMaterialHandle handle)
        {
            auto* rctx = lookupRenderContext(user_state);
            if (!rctx)
                return;
            if (!ensureGlobalMaterialResources(*rctx))
                return;
            auto* mat = rctx->globalRegistry().find<MaterialResources>();
            if (!mat || !mat->isInitialized())
                return;
            mat->remove(MaterialHandle{handle.index, handle.gen});
            forEachSceneOnServer(user_state, [](RenderScene& scene) {
                scene.invalidateGraph(EGraphInvalidationReason::MATERIAL_LAYOUT);
            }
            );
        } // anonymous namespace (helpers)
    }
    void handleUploadGraphMaterial(GeneralRenderServer::Dispatcher::Ctx& ctx, const UploadGraphMaterialPayload& p)
    {
        auto desc_bytes = resolveExternalData(ctx.program, p.graph_desc);
        const auto* data = reinterpret_cast<const GraphMaterialData*>(desc_bytes.data());
        const RMaterialHandle h = serverUploadGraphMaterial(
            ctx.user_state,
            p.asset_id,
            *data,
            p.graph_gbuffer_shader,
            p.graph_forward_shader,
            p.shader_key,
            p.alpha_mode,
            p.double_sided != 0u
        );
        replyToCurrent<UploadGraphMaterialPayload>(ctx, MaterialUploadedReply{h, h.isNull() ? 1u : 0u});
    }

    void handleModifyGraphMaterial(GeneralRenderServer::Dispatcher::Ctx& ctx, const ModifyGraphMaterialPayload& p)
    {
        auto desc_bytes = resolveBlob(ctx.program, p.graph_desc);
        const auto* data = reinterpret_cast<const GraphMaterialData*>(desc_bytes.data());
        serverModifyGraphMaterial(ctx.user_state, p.handle, *data);
    }

    void handleDestroyMaterial(GeneralRenderServer::Dispatcher::Ctx& ctx, const DestroyMaterialPayload& p)
    {
        serverDestroyMaterial(ctx.user_state, p.handle);
    }

} // namespace lux::render
