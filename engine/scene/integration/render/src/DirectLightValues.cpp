#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/scene/render/DirectLightValues.hpp>

namespace lux::scene::detail
{
    using namespace lux::render;

    LightShadowInputs lightShadowInputs(const rdesc::LightDescription& source) noexcept
    {
        if (!source.cast_shadow)
        {
            return {};
        }
        LightShadowInputs result{true, source.shadow_bias, source.shadow_normal_bias};
        if (source.type == rdesc::ELightType::DIRECTIONAL)
        {
            result.cascade_count = source.cascade_count;
            result.cascade_splits = source.cascade_splits;
        }
        return result;
    }

    rdesc::LightDescription compiledLightDescription(const LightRenderResourceComponent& resource) noexcept
    {
        rdesc::LightDescription result;
        result.type = resource.type;
        result.cast_shadow = resource.shadows.enabled;
        result.shadow_bias = resource.shadows.bias;
        result.shadow_normal_bias = resource.shadows.normal_bias;
        result.cascade_count = resource.shadows.cascade_count;
        result.cascade_splits = resource.shadows.cascade_splits;
        return result;
    }

    RenderResult<VDirectLight> lightValue(const rdesc::LightDescription& light, const Eigen::Affine3d& world) noexcept
    {
        const auto position = world.translation().cast<float>().eval();
        const auto direction = (-world.linear().col(2)).normalized().cast<float>().eval();
        const std::array<float, 4> radiance{
            light.color[0] * light.intensity,
            light.color[1] * light.intensity,
            light.color[2] * light.intensity,
            0
        };
        switch (light.type)
        {
        case rdesc::ELightType::DIRECTIONAL:
            return VDirectLight{DirectionalLightGpu{radiance, {direction.x(), direction.y(), direction.z(), 0}}};
        case rdesc::ELightType::POINT:
            return VDirectLight{PointLightGpu{
                {position.x(), position.y(), position.z(), light.range},
                radiance,
                {light.attenuation_constant, light.attenuation_linear, light.attenuation_quadratic, 0}
            }};
        case rdesc::ELightType::SPOT:
            return VDirectLight{SpotLightGpu{
                {position.x(), position.y(), position.z(), light.range},
                radiance,
                {direction.x(), direction.y(), direction.z(), std::cos(light.outer_cone_angle)},
                {light.attenuation_constant,
                 light.attenuation_linear,
                 light.attenuation_quadratic,
                 std::cos(light.inner_cone_angle)}
            }};
        default:
            return cxx::unexpected(RenderError{kRuntimeUnsupported});
        }
    }

} // namespace lux::scene::detail
