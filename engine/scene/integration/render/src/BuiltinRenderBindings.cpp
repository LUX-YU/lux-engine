#include <lux/engine/scene/render/BuiltinRenderBindings.hpp>
#include <lux/engine/scene/render/CameraInputOperation.hpp>
#include <lux/engine/scene/render/CameraViewBinding.hpp>
#include <lux/engine/scene/render/DirectLightBinding.hpp>
#include <lux/engine/scene/render/DirectLightOperation.hpp>
#include <lux/engine/scene/render/LitSceneInputOperation.hpp>
#include <lux/engine/scene/render/MaterialRenderResourceComponent.hpp>
#include <lux/engine/scene/render/MeshInstanceRenderResourceComponent.hpp>
#include <lux/engine/scene/render/MeshRenderBinding.hpp>
#include <lux/engine/scene/render/MeshRenderResourceComponent.hpp>
#include <lux/engine/scene/render/RasterTransformOperation.hpp>

namespace lux::scene
{
    RenderBindingRegistrationResult registerBuiltinRenderBindings(RenderBindingDeclarations& declarations) noexcept
    {
        auto result = registerComponentBinding<MeshRenderBinding>(declarations);
        if (!result)
        {
            return result;
        }
        result = registerComponentBinding<DirectLightBinding>(declarations);
        if (!result)
        {
            return result;
        }
        result = registerCapabilityOperation<RasterTransformOperation>(declarations);
        if (!result)
        {
            return result;
        }
        result = registerCapabilityOperation<DirectLightOperation>(declarations);
        if (!result)
        {
            return result;
        }
        result = registerCapabilityOperation<LitSceneInputOperation>(declarations);
        if (!result)
        {
            return result;
        }
        result = registerComponentBinding<CameraViewBinding>(declarations);
        if (!result)
        {
            return result;
        }
        return registerCapabilityOperation<CameraInputOperation>(declarations);
    }
} // namespace lux::scene
