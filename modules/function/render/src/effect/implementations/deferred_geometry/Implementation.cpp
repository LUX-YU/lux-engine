#include <lux/engine/render/resource/instance/Resource.hpp>
#include <lux/engine/render/effect/implementations/mesh_visibility/MeshCulling.hpp>
#include <lux/engine/render/assembly/BuiltinEffects.hpp>
#include <lux/engine/render/effect/capabilities/DeferredGeometry.hpp>
#include <lux/engine/render/effect/capabilities/MeshVisibility.hpp>

namespace lux::render
{
    namespace
    {
        template <bool CounterClockwise>
        RenderResult<void> build(EffectCompileContext& context, bool frustum_culling) noexcept
        {
            if (!context.outputDemanded(DeferredGeometryPorts::surface.semantic))
            {
                auto cull = detail::appendCameraVisibility(context, {}, frustum_culling);
                if (!cull) return cxx::unexpected(cull.error());
                return {};
            }
            const auto key = "mesh-visibility/" + std::to_string(context.nodeId().value) + "/";
            detail::MeshRasterAttachments raster{rdesc::EMaterialPurpose::GBUFFER, CounterClockwise, {}};
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
                if (is_depth) raster.work.depth = RasterAttachment{{{*attachment, 0}}};
                else raster.work.colors.push_back({{{*attachment, 0}}});
            }
            const auto drawn = detail::appendCameraVisibility(context, std::move(raster), frustum_culling);
            if (!drawn) return cxx::unexpected(drawn.error());
            EffectResourceBundle bundle;
            for (std::size_t i = 0; i < GBufferBundle::members.size(); ++i)
                bundle.push_back({effectPortSemantic(GBufferBundle::members[i]), drawn->written[i]});
            return context.publish(DeferredGeometryPorts::surface.semantic, std::move(bundle));
        }
    }

    std::array<ImplementationEntry, 2> detail::gBufferImplementations() noexcept
    {
        const auto variant = [](std::string_view name, ImplementationEntry::Create create)
        {
            return ImplementationEntry{
                .capability = capabilities::DeferredGeometry.id,
                .identity = {std::string{name}, capabilities::DeferredGeometry.version},
                .supported_outputs = {DeferredGeometryPorts::surface.semantic, DeferredGeometryPorts::draws.semantic},
                .create = create
            };
        };
        return {
            variant("lux.render.gbuffer.indexed", CameraVisibility<build<true>>::create),
            variant("lux.render.gbuffer.indexed.clockwise", CameraVisibility<build<false>>::create)
        };
    }
}
