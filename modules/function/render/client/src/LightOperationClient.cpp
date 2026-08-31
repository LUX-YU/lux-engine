#include <lux/engine/function/render/client/features/light/LightOperation.hpp>

#include <lux/engine/function/render/client/genops/LightOperation.ops.hpp>

namespace lux::render
{
    void lightUpsert(
        LightProxy proxy,
        RenderSceneId scene_id,
        RenderEntityId entity,
        const LightDescriptor& descriptor,
        std::uint32_t transition_milliseconds
    )
    {
        auto payload = toLightPayload(scene_id, entity, descriptor);
        payload.transition_milliseconds = transition_milliseconds;
        proxy.upsertLight(payload);
    }
} // namespace lux::render
