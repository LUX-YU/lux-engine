#include <lux/engine/render/features/FeatureAssembly.hpp>
#include <lux/engine/render/features/FullscreenRaster.hpp>
#include <lux/engine/render/features/GBufferFeature.hpp>
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
        constexpr std::array<std::string_view, 4> ports{"albedo_metallic", "normal_roughness", "emissive_ao", "depth"};

        RenderResult<void> describeGBuffer(const void* value, std::string_view, FeaturePorts& result) noexcept
        {
            const auto& config = *static_cast<const GBufferFeatureConfig*>(value);
            if (auto valid = config.target.validate(); !valid)
            {
                return valid;
            }
            if (config.target.samples != 1)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            for (std::size_t i = 0; i < config.groups.size(); ++i)
            {
                const auto& group = config.groups[i];
                const auto* use = group.program.find(rdesc::EMaterialPurpose::GBUFFER);
                const bool supported_model =
                    group.program.shading_model == rdesc::ELightingTechnique::UNLIT ||
                    group.program.shading_model == rdesc::ELightingTechnique::PBR_METALLIC_ROUGHNESS;
                if (!supported_model || !use || use->output_semantic != rdesc::GBufferLayout::semantic)
                {
                    return cxx::unexpected(RenderError{kFeatureContract});
                }
                if (auto valid = detail::describeMeshGroup(
                        group,
                        "group" + std::to_string(i) + ".",
                        rdesc::EMaterialPurpose::GBUFFER,
                        result
                    );
                    !valid)
                {
                    return valid;
                }
            }
            for (std::size_t i = 0; i < ports.size(); ++i)
            {
                const auto format =
                    i == 3 ? rdesc::GBufferLayout::depth_format : rdesc::GBufferLayout::color_formats[i];
                GraphImageDesc image{config.target.width, config.target.height, 1, 1, format};
                image.extent.target_relative = true;
                FeaturePort port{std::string(ports[i]), EFeaturePort::OUTPUT, image};
                port.semantic = std::string(rdesc::GBufferLayout::semantic) + "." + std::string(ports[i]);
                result.resources.push_back(std::move(port));
            }
            return {};
        }

        template <bool Paged>
        RenderResult<void> buildGBuffer(const void* value, std::string_view key, FeatureBuildContext& context) noexcept
        {
            const auto& config = *static_cast<const GBufferFeatureConfig*>(value);
            std::array<ResourceVersion, 4> versions;
            for (std::size_t i = 0; i < ports.size(); ++i)
            {
                versions[i] = context.resource(ports[i]);
            }
            if (config.groups.empty())
            {
                TransferWork work;
                for (auto version : versions)
                {
                    work.operations.push_back(ClearImage{{version}});
                }
                auto pass = context.graph.addTransferPass(
                    featureKey(GBufferFeatureDescriptor::name, key, "clear"),
                    std::move(work)
                );
                if (!pass)
                {
                    return cxx::unexpected(pass.error());
                }
                std::ranges::copy(pass->written, versions.begin());
            }
            for (std::size_t i = 0; i < config.groups.size(); ++i)
            {
                auto draw = detail::buildMeshGroup(
                    config.groups[i],
                    "group" + std::to_string(i) + ".",
                    rdesc::EMaterialPurpose::GBUFFER,
                    Paged,
                    context
                );
                if (!draw)
                {
                    return cxx::unexpected(draw.error());
                }
                draw->interface.parameters.instance_layout = config.input_layout;
                const auto load = i == 0 ? EAttachmentLoad::CLEAR : EAttachmentLoad::LOAD;
                for (std::size_t color = 0; color < 3; ++color)
                {
                    draw->colors.push_back({{versions[color]}, load});
                }
                draw->depth = RasterAttachment{{versions[3]}, load};
                auto pass = context.graph.addRasterPass(
                    featureKey(GBufferFeatureDescriptor::name, key, "group" + std::to_string(i)),
                    std::move(*draw)
                );
                if (!pass)
                {
                    return cxx::unexpected(pass.error());
                }
                std::ranges::copy(pass->written, versions.begin());
            }
            for (std::size_t i = 0; i < ports.size(); ++i)
            {
                if (auto published = context.publish(ports[i], versions[i]); !published)
                {
                    return published;
                }
            }
            return {};
        }
    } // namespace

    RenderResult<std::size_t> ConfigValueTraits<GBufferFeatureConfig>::retainedBytes(const GBufferFeatureConfig& value
    ) noexcept
    {
        auto bytes = sizeof(value) + value.groups.capacity() * sizeof(RasterMeshGroup) +
                     detail::parameterLayoutBytes(value.input_layout);
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

    const FeatureDescriptor GBufferFeatureDescriptor::descriptor{
        name,
        cppType<Config>(),
        "World-space material surfaces in a linear MRT GBuffer with forward depth.",
        describeGBuffer
    };

    template <>
    RenderResult<void> RenderServer::prepareFeature<GBufferFeatureDescriptor>(
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
        static const FeatureGraphOps contiguous{buildGBuffer<false>}, paged{buildGBuffer<true>};
        batch.addFeature({&GBufferFeatureDescriptor::descriptor, pagedInstances() ? &paged : &contiguous});
        return {};
    }

    template LUX_RENDER_PUBLIC RenderResult<FeatureId> RenderServer::registerFeature<GBufferFeatureDescriptor>(
        std::string_view
    ) noexcept;
} // namespace lux::render
