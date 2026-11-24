#include <lux/engine/render/effect/capabilities/MeshVisibility.hpp>
#include <lux/engine/render/resource/instance/Resource.hpp>
#include <lux/engine/render/effect/implementations/mesh_visibility/MeshCulling.hpp>
#include <lux/engine/render/assembly/BuiltinEffects.hpp>
#include <lux/engine/render/effect/capabilities/ForwardGeometry.hpp>
#include <lux/engine/render/effect/capabilities/DepthReduction.hpp>
#include <lux/engine/render/effect/capabilities/ToneMapping.hpp>

namespace lux::render
{
    namespace
    {
        template <bool Color, bool CounterClockwise>
        RenderResult<void> build(EffectCompileContext& context, bool frustum_culling) noexcept
        {
            const auto key = "mesh-visibility/" + std::to_string(context.nodeId().value) + "/";
            constexpr auto purpose = Color ? rdesc::EMaterialPurpose::FORWARD : rdesc::EMaterialPurpose::SHADOW;
            detail::MeshRasterAttachments raster{purpose, CounterClockwise, {}};
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
            raster.work.depth = RasterAttachment{{{*depth, 0}}};
            if constexpr (Color)
            {
                GraphImageDesc color_image{0, 0, 1, 1, target.format};
                color_image.extent.source = EImageExtentSource::TARGET;
                const auto color = context.declareResource({key + "color", color_image});
                if (!color) return cxx::unexpected(color.error());
                raster.work.colors.push_back({{{*color, 0}}});
            }
            const auto drawn = detail::appendCameraVisibility(context, std::move(raster), frustum_culling);
            if (!drawn) return cxx::unexpected(drawn.error());
            constexpr auto output = Color ? ForwardGeometryPorts::color.semantic : ForwardGeometryPorts::depth.semantic;
            return context.publish(output, {{output, drawn->written.front()}});
        }
    }

    std::array<ImplementationEntry, 4> detail::meshRasterImplementations() noexcept
    {
        const auto variant = [](std::string_view name, ImplementationEntry::Create create, graph::PinSemanticId output)
        {
            return ImplementationEntry{
                .capability = capabilities::ForwardGeometry.id,
                .identity = {std::string{name}, capabilities::ForwardGeometry.version},
                .supported_outputs = {output},
                .create = create
            };
        };
        return {
            variant("lux.render.mesh_raster.forward", CameraVisibility<build<true, true>>::create, ForwardGeometryPorts::color.semantic),
            variant("lux.render.mesh_raster.forward.clockwise", CameraVisibility<build<true, false>>::create, ForwardGeometryPorts::color.semantic),
            variant("lux.render.mesh_raster.depth", CameraVisibility<build<false, true>>::create, ForwardGeometryPorts::depth.semantic),
            variant("lux.render.mesh_raster.depth.clockwise", CameraVisibility<build<false, false>>::create, ForwardGeometryPorts::depth.semantic)
        };
    }
}
