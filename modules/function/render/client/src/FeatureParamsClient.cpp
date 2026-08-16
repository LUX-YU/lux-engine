#include <lux/engine/function/render/client/protocol/FeatureParamsOperation.hpp>

#include <lux/engine/function/render/client/RenderFrameSession.hpp>

#include <cstddef>
#include <span>

namespace lux::render
{
    void FeatureParamsProxy::setParams(
        RenderSceneId scene,
        FeatureHandle feature,
        TypeId op,
        const void* blob,
        std::size_t size
    )
    {
        if (op == kInvalidTypeId || blob == nullptr || size == 0)
            return;

        auto& builder = session_->builder();
        SetFeatureParamsPayload payload{};
        payload.scene   = scene;
        payload.feature = feature;
        payload.params  = builder.pushBlob(
            std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(blob),
                size
            },
            16u
        );
        builder.push(opcodes::CommandOp, op, payload);
    }
} // namespace lux::render
