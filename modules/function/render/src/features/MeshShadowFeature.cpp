#include <lux/engine/render/extensions/features/FeatureAssembly.hpp>
#include <lux/engine/render/features/BuiltinFeatures.hpp>
#include <lux/engine/render/features/MeshShadowFeature.hpp>
#include <lux/engine/render/features/RasterMesh.hpp>

namespace lux::render
{
    RenderResult<void>
    detail::MeshShadowFeature::Ports::describe(const Config* value, std::string_view, FeaturePorts& ports) noexcept
    {
        const auto& config = *static_cast<const MeshShadowFeatureConfig*>(value);
        if (config.shape.resolution == 0)
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        detail::describeMeshDrawInputs(ports);
        for (auto& port : ports.resources)
        {
            if (port.key == "commands" || port.key == "counts")
                std::get<GraphBufferDesc>(port.description).size *= config.shape.view_count;
        }
        FeaturePort views{
            "views", EFeaturePort::INPUT, GraphBufferDesc{config.shape.view_count * sizeof(MeshVisibilityView)}
        };
        views.semantic = kMeshVisibilityViews;
        ports.resources.push_back(std::move(views));
        FeaturePort states{
            "raster_states", EFeaturePort::INPUT, GraphBufferDesc{config.shape.view_count * sizeof(RasterState)}
        };
        states.semantic = "lux.raster.state.v1";
        ports.resources.push_back(std::move(states));
        FeaturePort depth{"depth", EFeaturePort::OUTPUT, config.shape.image()};
        depth.semantic = kShadowDepth;
        ports.resources.push_back(std::move(depth));
        return {};
    }

    template <bool Paged>
    RenderResult<void>
    detail::MeshShadowFeature::build(const Config*, std::string_view key, FeatureBuildContext& context) noexcept
    {
        auto draw = detail::meshRasterStage({}, context);
        auto& stage = std::get<RasterPassTemplate>(draw.program);
        stage.parameters.fields.clear();
        stage.resources.push_back({context.resource("views")});
        stage.resources.back().usage = EGraphUse::STORAGE_READ;
        // Raster states are CPU draw parameters. The draw binding declares their
        // preparation dependency; they must not become a GPU Buffer access or barrier.
        draw.depth = RasterAttachment{{context.resource("depth")}};
        auto pass =
            context.graph.addRasterPass(featureKey(detail::MeshShadowFeature::name, key, "draw"), std::move(draw));
        if (!pass)
        {
            return cxx::unexpected(pass.error());
        }
        if (auto bound = detail::bindMeshDraws(
                context,
                pass->pass,
                rdesc::EMaterialPurpose::SHADOW,
                Paged,
                context.resource("views").resource,
                context.resource("raster_states").resource
            );
            !bound)
        {
            return bound;
        }
        return context.publish("depth", pass->written.front());
    }

    RenderResult<std::size_t>
    ConfigValueTraits<MeshShadowFeatureConfig>::retainedBytes(const MeshShadowFeatureConfig& value) noexcept
    {
        return sizeof(value);
    }

    template RenderResult<void>
    detail::MeshShadowFeature::build<false>(const Config*, std::string_view, FeatureBuildContext&) noexcept;

    template RenderResult<void>
    detail::MeshShadowFeature::build<true>(const Config*, std::string_view, FeatureBuildContext&) noexcept;

} // namespace lux::render
