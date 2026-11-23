#include <lux/engine/render/effect/builtin/MeshVisibilityBuild.hpp>
#include <lux/engine/render/effect/builtin/Implementations.hpp>
#include <lux/engine/render/effect/builtin/MeshRaster.hpp>
#include <lux/engine/render/effect/builtin/DepthPyramid.hpp>
#include <lux/engine/render/effect/builtin/ToneMapping.hpp>

namespace lux::render
{
    namespace
    {
        template <bool Color>
        RenderResult<GraphPassId>
        appendSurface(EffectCompileContext& context, const detail::MeshAttachmentInputs& inputs, RasterWork raster) noexcept
        {
            const auto& key = inputs.key;
            const auto& target = context.target();
            if constexpr (Color)
            {
                const bool supported = target.format == rdesc::ETextureFormat::RGBA8_UNORM ||
                    target.format == rdesc::ETextureFormat::RGBA8_SRGB;
                if (!supported || target.samples != 1)
                    return cxx::unexpected(RenderError{kDeviceUnsupported, {context.nodeId().value}});
            }
            GraphImageDesc depth_image{0, 0, 1, 1, rdesc::ETextureFormat::D32_SFLOAT};
            depth_image.extent.source = EImageExtentSource::TARGET;
            const auto depth = context.declareResource({key + "depth", depth_image});
            if (!depth) return cxx::unexpected(depth.error());
            raster.depth = RasterAttachment{{{*depth, 0}}};
            if constexpr (Color)
            {
                GraphImageDesc color_image{0, 0, 1, 1, target.format};
                color_image.extent.source = EImageExtentSource::TARGET;
                const auto color = context.declareResource({key + "color", color_image});
                if (!color) return cxx::unexpected(color.error());
                raster.colors.push_back({{{*color, 0}}});
            }
            const auto drawn = context.addRasterPass(key + "raster", std::move(raster));
            if (!drawn) return cxx::unexpected(drawn.error());
            constexpr auto output = Color ? MeshRaster::color : MeshRaster::depth;
            if (auto published = context.publish(output, {{output, drawn->written.front()}}); !published)
                return cxx::unexpected(published.error());
            return drawn->pass;
        }

        template <bool Color, bool CounterClockwise>
        RenderResult<void> build(EffectCompileContext& context, std::string_view) noexcept
        {
            constexpr detail::MeshSurfaceOutput output{
                Color ? MeshRaster::color : MeshRaster::depth,
                Color ? rdesc::EMaterialPurpose::FORWARD : rdesc::EMaterialPurpose::SHADOW,
                appendSurface<Color>
            };
            return detail::appendCameraVisibility(context, &output, CounterClockwise);
        }
    }

    EffectNodeDefinition MeshRaster::definition() noexcept
    {
        EffectNodeDefinition definition;
        definition.identity = {graph::nodeTypeId(capability), std::string{capability}, version};
        definition.display_name = "Mesh Raster";
        definition.ports = {
            {color, "color", {std::string{ToneMapping::display_type}, 1}, graph::EPinDirection::OUTPUT, UINT8_MAX},
            {depth, "depth", {std::string{DepthPyramid::depth_type}, 1}, graph::EPinDirection::OUTPUT, UINT8_MAX}
        };
        return definition;
    }

    std::array<EffectImplementation, 4> detail::meshRasterImplementations() noexcept
    {
        const auto variant = [](std::string_view name, EffectImplementation::Build build, graph::PinSemanticId output)
        {
            return EffectImplementation{
                .capability = graph::nodeTypeId(MeshRaster::capability),
                .identity = {std::string{name}, MeshRaster::version},
                .supported_outputs = {output},
                .initialize = initializeVisibility,
                .build = build
            };
        };
        return {
            variant(MeshRaster::forward, build<true, true>, MeshRaster::color),
            variant(MeshRaster::forward_clockwise, build<true, false>, MeshRaster::color),
            variant(MeshRaster::depth_only, build<false, true>, MeshRaster::depth),
            variant(MeshRaster::depth_only_clockwise, build<false, false>, MeshRaster::depth)
        };
    }
}
