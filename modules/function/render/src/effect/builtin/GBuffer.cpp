#include <lux/engine/render/effect/builtin/MeshVisibilityBuild.hpp>
#include <lux/engine/render/effect/builtin/Implementations.hpp>
#include <lux/engine/render/effect/builtin/GBuffer.hpp>
#include <lux/engine/render/effect/builtin/MeshVisibility.hpp>

namespace lux::render
{
    namespace
    {
        RenderResult<GraphPassId>
        appendSurface(EffectCompileContext& context, const detail::MeshAttachmentInputs& inputs, RasterWork raster) noexcept
        {
            const auto& key = inputs.key;
            if (context.target().samples != 1)
                return cxx::unexpected(RenderError{kDeviceUnsupported, {context.nodeId().value}});
            for (std::size_t i = 0; i < GBufferBundle::members.size(); ++i)
            {
                const bool is_depth = i == rdesc::GBufferLayout::color_formats.size();
                const auto format = GBufferBundle::formats[i];
                GraphImageDesc image{0, 0, 1, 1, format};
                image.extent.source = EImageExtentSource::TARGET;
                const auto attachment = context.declareResource({key + std::string{GBufferBundle::members[i]}, image});
                if (!attachment) return cxx::unexpected(attachment.error());
                if (is_depth) raster.depth = RasterAttachment{{{*attachment, 0}}};
                else raster.colors.push_back({{{*attachment, 0}}});
            }
            const auto drawn = context.addRasterPass(key + "raster", std::move(raster));
            if (!drawn) return cxx::unexpected(drawn.error());
            EffectResourceBundle bundle;
            for (std::size_t i = 0; i < GBufferBundle::members.size(); ++i)
                bundle.push_back({effectPortSemantic(GBufferBundle::members[i]), drawn->written[i]});
            if (auto published = context.publish(GBuffer::surface, std::move(bundle)); !published)
                return cxx::unexpected(published.error());
            return drawn->pass;
        }

        template <bool CounterClockwise>
        RenderResult<void> build(EffectCompileContext& context, std::string_view) noexcept
        {
            constexpr detail::MeshSurfaceOutput output{GBuffer::surface, rdesc::EMaterialPurpose::GBUFFER, appendSurface};
            return detail::appendCameraVisibility(context, &output, CounterClockwise);
        }
    }

    EffectNodeDefinition GBuffer::definition() noexcept
    {
        EffectNodeDefinition definition;
        definition.identity = {graph::nodeTypeId(capability), std::string{capability}, version};
        definition.display_name = "GBuffer";
        definition.ports = {
            {surface, "surface", {std::string{GBufferBundle::type}, GBufferBundle::version},
                graph::EPinDirection::OUTPUT, UINT8_MAX},
            {draws, "draws", {std::string{MeshVisibility::draws_type}, 1},
                graph::EPinDirection::OUTPUT, UINT8_MAX}
        };
        return definition;
    }

    std::array<EffectImplementation, 2> detail::gBufferImplementations() noexcept
    {
        const auto variant = [](std::string_view name, EffectImplementation::Build build)
        {
            return EffectImplementation{
                .capability = graph::nodeTypeId(GBuffer::capability),
                .identity = {std::string{name}, GBuffer::version},
                .supported_outputs = {GBuffer::surface, GBuffer::draws},
                .initialize = initializeVisibility,
                .build = build
            };
        };
        return {
            variant(GBuffer::indexed, build<true>),
            variant(GBuffer::indexed_clockwise, build<false>)
        };
    }
}
