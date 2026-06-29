// ============================================================================
//  FeatureParamsHandlers.cpp — shared handler + registration helper + client
//  proxy for the generic per-feature setParams op (FeatureParamsOperation.hpp).
// ============================================================================

#include <lux/engine/render/renderer/features/FeatureParamsOperation.hpp>

#include <lux/engine/render/comm/server/RenderServer.hpp>   // Dispatcher, Ctx, resolveBlob, allocateAndRegisterUnary
#include <lux/engine/render/comm/RenderProtocol.hpp>          // opcodes
#include <lux/engine/render/comm/client/RenderSession.hpp>    // builder
#include <lux/engine/render/scene/RenderScene.hpp>            // getFeature / invalidateGraph
#include <lux/engine/render/RenderFeature.hpp>                // applyParams / EParamApply

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace lux::render
{
    using Dispatcher = GeneralRenderServer::Dispatcher;
    using Ctx        = Dispatcher::Ctx;

    // Exported by the server for feature operation handlers (forward-declared, the
    // grid-handler convention — avoids pulling RenderServerImpl.hpp into a feature).
    RenderScene* lookupScene(void* user_state, RenderSceneId scene_id);

    namespace
    {
        // Route a reflected param blob to its feature + apply it. Mirrors what the
        // per-feature typed handlers (e.g. Tonemap's) used to do, but generic: the
        // feature's applyParams() decides the verdict (HOT / NEEDS_RECOMPILE / ...).
        void handleSetFeatureParams(Ctx& ctx, const SetFeatureParamsPayload& p)
        {
            auto* sc = lookupScene(ctx.user_state, p.scene);
            if (!sc) return;
            auto* f = sc->getFeature(p.feature);
            if (!f) return;
            auto blob = resolveBlob(ctx.program, p.params);
            const auto verdict = f->applyParams(blob.data(), blob.size());
            if (verdict == RenderFeature::EParamApply::NEEDS_RECOMPILE ||
                verdict == RenderFeature::EParamApply::NEEDS_RECREATE)
                sc->invalidateGraph();
        }
    } // namespace

    TypeId registerFeatureParamsOp(void* dispatcher)
    {
        auto& d = *static_cast<Dispatcher*>(dispatcher);
        // Empty op-name on purpose: the panel addresses this op BY id (from the
        // factory's param_set_op_index), not by name — and a shared name would
        // collide across features in the dispatcher's global name index.
        return d.allocateAndRegisterUnary<SetFeatureParamsPayload, &handleSetFeatureParams>(
            opcodes::CommandOp, "");
    }

    void FeatureParamsProxy::setParams(RenderSceneId scene, FeatureHandle feature, TypeId op,
                                       const void* blob, std::size_t size)
    {
        if (op == kInvalidTypeId || blob == nullptr || size == 0) return;
        auto& b = session_->builder();
        SetFeatureParamsPayload p{};
        p.scene   = scene;
        p.feature = feature;
        p.params  = b.pushBlob(
            std::span<const std::byte>{reinterpret_cast<const std::byte*>(blob), size}, 16u);
        b.push(opcodes::CommandOp, op, p);
    }

} // namespace lux::render
