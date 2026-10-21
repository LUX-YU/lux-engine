#include <lux/engine/render/features/MeshShadowFeature.hpp>

#include <cmath>
#include <lux/engine/render/features/FeatureAssembly.hpp>
#include <lux/engine/render/features/RasterMesh.hpp>
#include <lux/engine/render/runtime/Registration.hpp>
#include <lux/engine/render/storage/IndexedTriangleMeshCommandHandler.hpp>
#include <lux/engine/render/storage/RasterMaterialTableCommandHandler.hpp>
#include <lux/engine/render/storage/RasterMeshInstanceCommandHandler.hpp>
#include <lux/engine/render/storage/SampledTexture2DCommandHandler.hpp>

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
                meshVisibleStride(config.candidate_capacity) / 4 * config.shape.view_count > UINT32_MAX;
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
                if (auto result =
                        detail::describeMeshGroup(config.groups[g], prefix, rdesc::EMaterialPurpose::SHADOW, ports);
                    !result)
                {
                    return result;
                }
                auto& members = *std::ranges::find(ports.resources, prefix + "members", &FeaturePort::key);
                members.direction = EFeaturePort::INPUT;
                members.description =
                    GraphBufferDesc{meshVisibleStride(config.candidate_capacity) * config.shape.view_count};
                members.origin = EGraphOrigin::TRANSIENT;
                members.initial_ranges.clear();
                members.semantic = kMeshVisibleMembers;
                ports.resources.push_back(
                    {prefix + "commands",
                     EFeaturePort::INPUT,
                     GraphBufferDesc{std::uint64_t(config.shape.view_count) * 20}}
                );
                ports.resources.push_back(
                    {prefix + "counts", EFeaturePort::INPUT, GraphBufferDesc{std::uint64_t(config.shape.view_count) * 4}
                    }
                );
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
                featureKey(MeshShadowFeatureDescriptor::name, key, "clear"),
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
                auto base = detail::buildMeshGroup(
                    config.groups[g],
                    prefix,
                    rdesc::EMaterialPurpose::SHADOW,
                    Paged,
                    context,
                    false
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
                    auto& members =
                        *std::ranges::find(work.interface.resources, "members", &ResourceParameter::shader_name);
                    const auto size = meshVisibleStride(config.candidate_capacity);
                    members.access.range = GraphBufferRange{size * view, size};
                    auto& draw = std::get<DrawIndexedIndirect>(work.draw);
                    draw.commands_resource = context.resource(prefix + "commands");
                    draw.commands_offset = std::uint64_t(view) * 20;
                    draw.count_resource = context.resource(prefix + "counts");
                    draw.count_offset = std::uint64_t(view) * 4;
                    work.depth = RasterAttachment{{depth, GraphImageRange{2, 0, 1, view, 1}}, EAttachmentLoad::LOAD};
                    auto pass = context.graph.addRasterPass(
                        featureKey(MeshShadowFeatureDescriptor::name, key, prefix + "slice" + std::to_string(view)),
                        std::move(work)
                    );
                    if (!pass)
                    {
                        return cxx::unexpected(pass.error());
                    }
                    depth = pass->written.front();
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

    const FeatureDescriptor MeshShadowFeatureDescriptor::descriptor{
        name,
        cppType<Config>(),
        "Shadow-purpose raster over explicit views and GPU-visible groups; owns only its depth output.",
        describeShadowRaster
    };

    template <>
    RenderResult<void> RenderServer::prepareFeature<MeshShadowFeatureDescriptor>(
        RegistrationBatch& batch,
        std::string_view
    ) noexcept
    {
        if (auto result = prepareStorage<SampledTexture2DStorageDescriptor>(batch, {}, true, {}); !result)
        {
            return result;
        }
        if (auto result = prepareStorage<IndexedTriangleMeshStorageDescriptor>(batch, {}, true, {}); !result)
        {
            return result;
        }
        if (auto result = prepareStorage<RasterMaterialTableStorageDescriptor>(batch, {}, true, {}); !result)
        {
            return result;
        }
        if (auto result = prepareStorage<RasterMeshInstanceStorageDescriptor>(batch, {}, true, {}); !result)
        {
            return result;
        }
        static const FeatureGraphOps contiguous{buildShadowRaster<false>}, paged{buildShadowRaster<true>};
        batch.addFeature({&MeshShadowFeatureDescriptor::descriptor, pagedInstances() ? &paged : &contiguous});
        return {};
    }

    template LUX_RENDER_PUBLIC RenderResult<FeatureId> RenderServer::registerFeature<MeshShadowFeatureDescriptor>(
        std::string_view
    ) noexcept;
} // namespace lux::render
