#include <lux/engine/render/core/Descriptors.hpp>
#include <lux/engine/render/core/Handles.hpp>

#if defined(LUX_PROBE_VULKAN)
#include <vulkan/vulkan.h>
#elif defined(LUX_PROBE_SCENE)
#include <lux/engine/scene/SceneSystem.hpp>
#elif defined(LUX_PROBE_RUNTIME)
#include <lux/engine/render/RenderRuntime.hpp>
#elif defined(LUX_PROBE_LEGACY)
#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>
#endif

int main()
{
#if defined(LUX_PROBE_ID)
    lux::render::FeatureTypeId id = lux::render::renderDataTypeId("test.value.v1");
    return id.isValid() ? 0 : 1;
#elif defined(LUX_PROBE_HANDLE)
    lux::render::RenderViewHandle handle = lux::render::RenderSceneId{0, 1};
    return handle.isValid() ? 0 : 1;
#else
    constexpr auto id = lux::render::renderDataTypeId("test.value.v1");
    const lux::render::RenderDataDescriptor descriptor{id, "test.value.v1", 1, 1, 4, 4};
    return lux::render::validateDescriptor(descriptor) ? 0 : 1;
#endif
}
