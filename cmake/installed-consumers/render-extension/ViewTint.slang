#include <lux/engine/render/resource/view/SceneView.slang>

[[vk::binding(0, 0)]] StructuredBuffer<SceneViewGpu> scene_view : register(t0, space0);

[shader("fragment")]
float4 main() : SV_Target0
{
    return scene_view[0].background;
}
