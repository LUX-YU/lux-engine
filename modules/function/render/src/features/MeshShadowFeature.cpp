#include <lux/engine/render/features/MeshShadowFeature.hpp>

#include <cmath>
#include <lux/engine/render/extensions/features/FeatureAssembly.hpp>
#include <lux/engine/render/extensions/storage/IndexedTriangleMesh.hpp>
#include <lux/engine/render/extensions/storage/RasterMaterialTable.hpp>
#include <lux/engine/render/extensions/storage/RasterMeshInstances.hpp>
#include <lux/engine/render/extensions/storage/SampledTexture2D.hpp>
#include <lux/engine/render/features/BuiltinFeatures.hpp>
#include <lux/engine/render/features/RasterMesh.hpp>

namespace lux::render
{
    namespace
    {
        RenderResult<void> describeShadowRaster(const void* value, std::string_view, FeaturePorts& ports) noexcept
        {
            const auto& config = *static_cast<const MeshShadowFeatureConfig*>(value);
            const bool invalid_shape = !config.shape.resolution || !config.shape.view_count ||
                                       !config.candidate_capacity || config.candidate_capacity > UINT32_MAX - 68;
            const bool oversized_stream =
                !invalid_shape && meshVisibleStride(config.candidate_capacity) / 4 >
                                      UINT32_MAX / (std::uint64_t(config.shape.view_count) * kMeshWindingCount);
            if (invalid_shape || oversized_stream)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            FeaturePort views{
                "views",
                EFeaturePort::INPUT,
                GraphBufferDesc{std::uint64_t(config.shape.view_count) * sizeof(MeshVisibilityView)}
            };
            views.semantic = kMeshVisibilityViews;
            ports.resources.push_back(std::move(views));
            FeaturePort state{
                "raster_states",
                EFeaturePort::INPUT,
                GraphBufferDesc{std::uint64_t(config.shape.view_count) * sizeof(RasterState)}
            };
            state.semantic = "lux.raster.state.v1";
            ports.resources.push_back(std::move(state));
            FeaturePort depth{"depth", EFeaturePort::OUTPUT, config.shape.image()};
            depth.semantic = kShadowDepth;
            ports.resources.push_back(std::move(depth));
            for (std::size_t g = 0; g < config.groups.size(); ++g)
            {
                const auto prefix = "g" + std::to_string(g) + ".";
                if (auto result = detail::describeMeshGroup(
                        config.groups[g],
                        prefix,
                        rdesc::EMaterialPurpose::SHADOW,
                        ports,
                        config.candidate_capacity,
                        config.shape.view_count
                    );
                    !result)
                {
                    return result;
                }
            }
            return {};
        }

        template <bool Paged>
        RenderResult<void> buildShadowRaster(
            const void* value,
            std::string_view key,
            FeatureBuildContext& context
        ) noexcept
        {
            const auto& config = *static_cast<const MeshShadowFeatureConfig*>(value);
            auto depth = context.resource("depth");
            // Clear is explicit even for zero groups or zero visible instances; no stale prior-FIF shadow data.
            auto clear = context.graph.addTransferPass(
                featureKey(detail::MeshShadowFeature::name, key, "clear"),
                TransferWork{{ClearImage{{depth}, {}, 1}}}
            );
            if (!clear)
            {
                return cxx::unexpected(clear.error());
            }
            depth = clear->written.front();
            for (std::size_t g = 0; g < config.groups.size(); ++g)
            {
                const auto prefix = "g" + std::to_string(g) + ".";
                for (std::uint32_t winding = 0; winding < kMeshWindingCount; ++winding)
                {
                    auto base = detail::buildMeshGroup(
                        config.groups[g],
                        prefix,
                        rdesc::EMaterialPurpose::SHADOW,
                        Paged,
                        context,
                        false,
                        winding != 0
                    );
                    if (!base)
                    {
                        return cxx::unexpected(base.error());
                    }
                    base->interface.parameters.instance_layout = {};
                    const MeshViewParameters identity;
                    auto bytes = meshViewParameterBytes(identity);
                    base->interface.parameters.values.assign(bytes.begin(), bytes.end());
                    base->viewport = RasterViewport{};
                    for (std::uint32_t view = 0; view < config.shape.view_count; ++view)
                    {
                        auto work = *base;
                        work.state_input = RasterStateInput{
                            context.resource("raster_states").resource,
                            view * static_cast<std::uint32_t>(sizeof(RasterState))
                        };
                        work.interface.parameters.fields = {
                            {"projection", context.resource("views").resource, "view" + std::to_string(view)}
                        };
                        detail::bindVisibleMeshDraw(work, context, prefix, config.candidate_capacity, view, winding);
                        work.depth =
                            RasterAttachment{{depth, GraphImageRange{2, 0, 1, view, 1}}, EAttachmentLoad::LOAD};
                        auto pass = context.graph.addRasterPass(
                            featureKey(
                                detail::MeshShadowFeature::name,
                                key,
                                prefix + "slice" + std::to_string(view) + ".winding" + std::to_string(winding)
                            ),
                            std::move(work)
                        );
                        if (!pass)
                        {
                            return cxx::unexpected(pass.error());
                        }
                        depth = pass->written.front();
                    }
                }
            }
            return context.publish("depth", depth);
        }
    } // namespace

    RenderResult<std::size_t> ConfigValueTraits<MeshShadowFeatureConfig>::retainedBytes(
        const MeshShadowFeatureConfig& value
    ) noexcept
    {
        auto bytes = sizeof(value) + value.groups.capacity() * sizeof(RasterMeshGroup);
        for (const auto& group : value.groups)
        {
            auto retained = ConfigValueTraits<RasterMeshGroup>::retainedBytes(group);
            if (!retained)
            {
                return cxx::unexpected(retained.error());
            }
            bytes += *retained - sizeof(group);
        }
        return bytes;
    }

    const FeatureGraphOps& detail::MeshShadowFeature::graph(bool paged_instances) noexcept
    {
        static const FeatureGraphOps contiguous{describeShadowRaster, buildShadowRaster<false>},
            paged{describeShadowRaster, buildShadowRaster<true>};
        return paged_instances ? paged : contiguous;
    }
} // namespace lux::render
