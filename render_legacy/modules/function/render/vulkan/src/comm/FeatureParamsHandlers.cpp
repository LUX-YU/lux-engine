// ============================================================================
//  FeatureParamsHandlers.cpp — server handler and registration helper for the
//  generic per-feature setParams operation.
// ============================================================================

#include <lux/engine/function/render/client/protocol/FeatureParamsOperation.hpp>

#include <lux/engine/render/comm/server/RenderServer.hpp> // Dispatcher, Ctx, resolveBlob, allocateAndRegisterUnary
#include <lux/engine/function/render/client/protocol/FeatureFactory.hpp> // FeatureFactory / GenericOkReply
#include <lux/engine/render/scene/RenderScene.hpp>                       // getFeature / invalidateGraph
#include <lux/engine/render/RenderFeature.hpp>                           // applyParams / EParamApply

#include <cstdint>
#include <utility>

namespace lux::render
{
    using Dispatcher = GeneralRenderServer::Dispatcher;
    using Ctx = Dispatcher::Ctx;

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
            if (!sc)
                return;
            auto* f = sc->getFeature(p.feature);
            if (!f)
                return;
            auto blob = resolveBlob(ctx.program, p.params);
            const auto verdict = f->applyParams(blob.data(), blob.size());
            if (verdict == RenderFeature::EParamApply::NEEDS_RECOMPILE ||
                verdict == RenderFeature::EParamApply::NEEDS_RECREATE)
                sc->invalidateGraph(EGraphInvalidationReason::FEATURE_TOPOLOGY);
        }
    } // namespace

    TypeId registerFeatureParamsOp(void* dispatcher)
    {
        auto& d = *static_cast<Dispatcher*>(dispatcher);
        // Empty op-name on purpose: the panel addresses this op BY id (from the
        // factory's param_set_op_index), not by name — and a shared name would
        // collide across features in the dispatcher's global name index.
        return d.allocateAndRegisterUnary<SetFeatureParamsPayload, &handleSetFeatureParams>(opcodes::CommandOp, "");
    }

} // namespace lux::render
