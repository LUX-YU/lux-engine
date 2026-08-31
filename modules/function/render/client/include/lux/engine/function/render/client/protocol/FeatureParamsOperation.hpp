#pragma once
// ============================================================================
//  FeatureParamsOperation.hpp — GENERIC per-feature param-apply op.
//
//  A config-bearing feature registers ONE setParams op (registerFeatureParamsOp,
//  in its register_ops_fn) whose payload is a GENERIC reflected-struct blob. The
//  shared handler routes by feature_handle + calls RenderFeature::applyParams(blob).
//  The settings panel pushes ANY feature's edited params through it — no per-feature
//  typed payload — so a plugin's reflected params are editable with zero compile-time
//  coupling. The op-id is still per-feature (dynamic, the grid pattern); only the
//  payload + handler are shared. (Chosen design: one generic setParams op per feature.)
// ============================================================================

#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp> // BlobRef, TypeId, opcodes
#include <lux/engine/function/render/client/core/FeatureHandle.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/engine/function/visibility.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace lux::render
{
    class RenderProgramSession;

    /// Generic "apply a reflected param-struct snapshot to a feature" command.
    struct SetFeatureParamsPayload
    {
        RenderSceneId scene{};
        FeatureHandle feature{};
        BlobRef params{}; ///< raw bytes of the feature's reflected param struct
    };
    static_assert(std::is_trivially_copyable_v<SetFeatureParamsPayload>);

    /// Register the shared setParams handler under a fresh DYNAMIC op-id (grid
    /// pattern). Called from a config-feature's register_ops_fn; record the returned
    /// id in the factory's ops[] and point FeatureFactory.param_set_op_index at it.
    LUX_FUNCTION_PUBLIC TypeId registerFeatureParamsOp(void* dispatcher);

    /// Client proxy — pushes a reflected param blob to a feature's setParams op.
    class LUX_FUNCTION_PUBLIC FeatureParamsProxy
    {
    public:
        explicit FeatureParamsProxy(RenderProgramSession& session) noexcept : session_(&session)
        {
        }

        /// @p op = the feature's setParams op-id (FeatureCatalog::paramSetOp(name)).
        /// No-op if @p op is invalid (feature exposes no editable params).
        void setParams(RenderSceneId scene, FeatureHandle feature, TypeId op, const void* blob, std::size_t size);

    private:
        RenderProgramSession* session_;
    };

} // namespace lux::render
