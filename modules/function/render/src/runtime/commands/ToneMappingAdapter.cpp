#include <lux/engine/render/features/postprocess/TonemapExposure.hpp>
#include <lux/engine/render/runtime/BuiltinRegistration.hpp>
#include <lux/engine/render/runtime/commands/CommandBinding.hpp>
#include <lux/engine/render/runtime/commands/EffectCommands.hpp>

namespace lux::render::detail
{
    namespace
    {
        void update(TonemapExposure& state, const SetToneMappingExposure::Request& request, AckResponse& response) noexcept
        {
            const auto result = state.setExposure(request.exposure);
            response.error = result ? RenderError{} : result.error();
        }
    }

    CommandBinding toneMappingExposureBinding() noexcept
    {
        return CommandBinding::declareForEffect<SetToneMappingExposure, TonemapExposure, &update>();
    }
}
