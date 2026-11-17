#include <lux/engine/render/extensions/features/FeatureAssembly.hpp>
#include <lux/engine/render/resources/mesh/Binding.hpp>
#include <lux/engine/render/resources/material/Binding.hpp>
#include <lux/engine/render/resources/instance/Binding.hpp>
#include <lux/engine/render/resources/texture/Binding.hpp>
#include <lux/engine/render/features/BuiltinFeatures.hpp>
#include <lux/engine/render/features/GBufferFeature.hpp>
#include <lux/engine/render/features/ParameterLayout.hpp>
#include <lux/engine/render/features/RasterMesh.hpp>

namespace lux::render
{
    constexpr std::array<std::string_view, 4> ports{"albedo_metallic", "normal_roughness", "emissive_ao", "depth"};

    RenderResult<void>
    detail::GBufferFeature::Ports::describe(const Config* value, std::string_view, FeaturePorts& result) noexcept
    {
        const auto& config = *static_cast<const GBufferFeatureConfig*>(value);
        if (auto valid = validateEffectOutput(config.output); !valid)
        {
            return valid;
        }
        if (config.output.samples != 1)
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        detail::describeMeshDrawInputs(result);
        for (std::size_t i = 0; i < ports.size(); ++i)
        {
            const auto format =
                i == 3 ? rdesc::GBufferLayout::depth_format : rdesc::GBufferLayout::color_formats[i];
            GraphImageDesc image{0, 0, 1, 1, format};
            image.extent.source = EImageExtentSource::TARGET;
            FeaturePort port{std::string(ports[i]), EFeaturePort::OUTPUT, image};
            port.semantic = std::string(rdesc::GBufferLayout::semantic) + "." + std::string(ports[i]);
            result.resources.push_back(std::move(port));
        }
        return {};
    }

    template <bool Paged>
    RenderResult<void>
    detail::GBufferFeature::build(const Config* value, std::string_view key, FeatureBuildContext& context) noexcept
    {
        const auto& config = *static_cast<const GBufferFeatureConfig*>(value);
        std::array<ResourceVersion, 4> versions;
        for (std::size_t i = 0; i < ports.size(); ++i)
        {
            versions[i] = context.resource(ports[i]);
        }
        auto draw = detail::meshRasterStage(config.input_layout, context);
        for (std::size_t color = 0; color < 3; ++color)
        {
            draw.colors.push_back({{versions[color]}});
        }
        draw.depth = RasterAttachment{{versions[3]}};
        auto pass =
            context.graph.addRasterPass(featureKey(detail::GBufferFeature::name, key, "draw"), std::move(draw));
        if (!pass)
        {
            return cxx::unexpected(pass.error());
        }
        if (auto bound = detail::bindMeshDraws(
                context,
                pass->pass,
                rdesc::EMaterialPurpose::GBUFFER,
                Paged,
                {},
                {},
                config.front_counter_clockwise
            );
            !bound)
        {
            return bound;
        }
        std::ranges::copy(pass->written, versions.begin());
        for (std::size_t i = 0; i < ports.size(); ++i)
        {
            if (auto published = context.publish(ports[i], versions[i]); !published)
            {
                return published;
            }
        }
        return {};
    }

    RenderResult<std::size_t>
    ConfigValueTraits<GBufferFeatureConfig>::retainedBytes(const GBufferFeatureConfig& value) noexcept
    {
        return sizeof(value) + detail::parameterLayoutBytes(value.input_layout);
    }

    template RenderResult<void>
    detail::GBufferFeature::build<false>(const Config*, std::string_view, FeatureBuildContext&) noexcept;

    template RenderResult<void>
    detail::GBufferFeature::build<true>(const Config*, std::string_view, FeatureBuildContext&) noexcept;

} // namespace lux::render
