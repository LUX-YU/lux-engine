#include <lux/engine/render/extensions/features/FeatureAssembly.hpp>
#include <lux/engine/render/resources/mesh/Binding.hpp>
#include <lux/engine/render/resources/material/Binding.hpp>
#include <lux/engine/render/resources/instance/Binding.hpp>
#include <lux/engine/render/resources/texture/Binding.hpp>
#include <lux/engine/render/features/BuiltinFeatures.hpp>
#include <lux/engine/render/features/mesh/GBufferFeature.hpp>
#include <lux/engine/render/features/mesh/RasterMesh.hpp>

namespace lux::render
{
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
        for (std::size_t i = 0; i < kGBufferPorts.size(); ++i)
        {
            const auto format =
                i == 3 ? rdesc::GBufferLayout::depth_format : rdesc::GBufferLayout::color_formats[i];
            GraphImageDesc image{0, 0, 1, 1, format};
            image.extent.source = EImageExtentSource::TARGET;
            FeaturePort port{std::string(kGBufferPorts[i]), EFeaturePort::OUTPUT, image};
            port.semantic = std::string(rdesc::GBufferLayout::semantic) + "." + std::string(kGBufferPorts[i]);
            result.resources.push_back(std::move(port));
        }
        return {};
    }

    RenderResult<void>
    detail::GBufferFeature::build(const Config* value, std::string_view key, FeatureBuildContext& context) noexcept
    {
        const auto& config = *static_cast<const GBufferFeatureConfig*>(value);
        std::array<ResourceVersion, 4> versions;
        for (std::size_t i = 0; i < kGBufferPorts.size(); ++i)
        {
            versions[i] = context.resource(kGBufferPorts[i]);
        }
        auto draw = detail::meshRasterStage(context);
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
                {},
                {},
                config.front_counter_clockwise
            );
            !bound)
        {
            return bound;
        }
        std::ranges::copy(pass->written, versions.begin());
        for (std::size_t i = 0; i < kGBufferPorts.size(); ++i)
        {
            if (auto published = context.publish(kGBufferPorts[i], versions[i]); !published)
            {
                return published;
            }
        }
        return {};
    }

} // namespace lux::render
