#include <lux/engine/function/render/client/features/light/LightOperation.hpp>

#include <lux/engine/function/render/client/genops/LightOperation.ops.hpp>

namespace lux::render
{
    RenderRequest<LightCreatedReply> lightCreate(
        LightProxy proxy,
        RenderSceneId scene_id,
        const LightDescriptor& descriptor,
        std::uint32_t transition_milliseconds
    )
    {
        auto payload = toLightPayload(scene_id, descriptor);
        payload.transition_milliseconds = transition_milliseconds;
        return proxy.createLight(payload);
    }

    void lightUpdate(
        LightProxy proxy,
        RenderSceneId scene_id,
        RLightHandle handle,
        const LightDescriptor& descriptor
    )
    {
        proxy.updateLight(
            toUpdateLightPayload(scene_id, handle, descriptor)
        );
    }
} // namespace lux::render
