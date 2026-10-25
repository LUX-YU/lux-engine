#include <lux/engine/scene/render/BuiltinRenderBindings.hpp>
#include <lux/engine/scene/render/DirectLightBinding.hpp>
#include <lux/engine/scene/render/DirectLightOperation.hpp>

namespace lux::scene
{
    RenderBindingRegistrationResult registerBuiltinRenderBindings(RenderBindingDeclarations& declarations) noexcept
    {
        auto binding = registerComponentBinding<DirectLightBinding>(declarations);
        if (!binding)
        {
            return binding;
        }
        return registerCapabilityOperation<DirectLightOperation>(declarations);
    }
} // namespace lux::scene
