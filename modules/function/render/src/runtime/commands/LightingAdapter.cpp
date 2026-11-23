#include <lux/engine/render/features/lighting/LightingState.hpp>
#include <lux/engine/render/runtime/BuiltinRegistration.hpp>
#include <lux/engine/render/runtime/commands/CommandBinding.hpp>
#include <lux/engine/render/runtime/commands/LightingCommands.hpp>

namespace lux::render::detail
{
    namespace
    {
        void update(LightingState& state, const SetLightingEnvironment::Request& request, AckResponse& response) noexcept
        {
            const auto result = state.setEnvironment(request.radiance);
            response.error = result ? RenderError{} : result.error();
        }
    }

    CommandBinding lightingEnvironmentBinding() noexcept
    {
        return CommandBinding::declareForEffect<SetLightingEnvironment, LightingState, &update>();
    }
}
