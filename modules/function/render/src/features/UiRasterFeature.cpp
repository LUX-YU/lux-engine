#include "UiOutputLinear.hpp"
#include "UiOutputSrgb.hpp"
#include "UiRasterFragment.hpp"
#include "UiRasterVertex.hpp"
#include <lux/engine/render/features/BuiltinFeatures.hpp>
#include <lux/engine/render/features/FullscreenRaster.hpp>
#include <lux/engine/render/features/UiRasterFeature.hpp>

namespace lux::render
{
    std::string uiImagePort(std::uint32_t index)
    {
        return "image" + std::to_string(index);
    }

    namespace
    {
        RenderResult<void> describeUi(const void* input, std::string_view, FeaturePorts& ports) noexcept
        {
            const auto& config = *static_cast<const UiRasterFeatureConfig*>(input);
            if (auto target = config.target.validate(); !target)
            {
                return target;
            }
            const bool invalid_images = config.images.empty() || config.images.size() > UINT32_MAX;
            const bool invalid_geometry = config.target.samples != 1 || config.index_type > EIndexType::UINT32;
            const bool invalid_output = config.target.format != rdesc::ETextureFormat::RGBA8_UNORM &&
                                        config.target.format != rdesc::ETextureFormat::RGBA8_SRGB &&
                                        config.target.format != rdesc::ETextureFormat::BGRA8_UNORM &&
                                        config.target.format != rdesc::ETextureFormat::BGRA8_SRGB;
            if (invalid_images || invalid_geometry || invalid_output)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            ports.resources.push_back(
                {"vertices",
                 EFeaturePort::OUTPUT,
                 GraphBufferDesc{sizeof(rdesc::UiVertex)},
                 EGraphOrigin::IMPORTED,
                 {GraphBufferRange{0, sizeof(rdesc::UiVertex)}}}
            );
            ports.resources.push_back(
                {"indices", EFeaturePort::OUTPUT, GraphBufferDesc{4}, EGraphOrigin::IMPORTED, {GraphBufferRange{0, 4}}}
            );
            GraphImageDesc color{config.target.width, config.target.height, 1, 1, rdesc::ETextureFormat::RGBA16_SFLOAT};
            color.extent.source = EImageExtentSource::TARGET;
            ports.resources.push_back({"linear_color", EFeaturePort::OUTPUT, color});
            color.format = config.target.format;
            ports.resources.push_back({"color", EFeaturePort::OUTPUT, color});
            for (std::uint32_t i = 0; i < config.images.size(); ++i)
            {
                auto result = std::visit(
                    [&](const auto& image) noexcept -> RenderResult<void>
                    {
                        const auto& description = image.description;
                        if (description.layers != 1 || description.samples != 1 ||
                            rdesc::textureAspectMask(description.format) != 1)
                        {
                            return cxx::unexpected(RenderError{kFeatureContract});
                        }
                        if constexpr (std::same_as<std::decay_t<decltype(image)>, UiTextureImage>)
                        {
                            ports.storage.global.push_back(
                                {uiImagePort(i),
                                 EFeaturePort::OUTPUT,
                                 description,
                                 EGraphOrigin::IMPORTED,
                                 {GraphImageRange{1, 0, description.mips, 0, 1}},
                                 StorageRequirement{StorageName(kTextureStorageType.name()), image.storage_key},
                                 EGraphUse::SAMPLED}
                            );
                        }
                        else
                        {
                            if (description.extent.source != EImageExtentSource::IMPORTED)
                            {
                                return cxx::unexpected(RenderError{kFeatureContract});
                            }
                            ports.resources.push_back({uiImagePort(i), EFeaturePort::INPUT, description});
                        }
                        return {};
                    },
                    config.images[i]
                );
                if (!result)
                {
                    return result;
                }
            }
            return {};
        }

        RenderResult<void> buildUi(const void* input, std::string_view key, FeatureBuildContext& context) noexcept
        {
            const auto& config = *static_cast<const UiRasterFeatureConfig*>(input);
            rdesc::SpirvArtifact shader;
            shader.stages.push_back(
                {rdesc::EShaderType::VERTEX, "main", {std::begin(kUiRasterVertex), std::end(kUiRasterVertex)}}
            );
            shader.stages.push_back(
                {rdesc::EShaderType::FRAGMENT, "main", {std::begin(kUiRasterFragment), std::end(kUiRasterFragment)}}
            );
            rdesc::ShaderInfo vertex;
            const bool valid_vertex = rdesc::ShaderInfo::deserialize(kUiRasterVertexReflection, vertex);
            const bool valid_fragment = rdesc::ShaderInfo::deserialize(kUiRasterFragmentReflection, shader.interface);
            if (!valid_vertex || !valid_fragment || vertex.push_constants.size() != 1 ||
                shader.interface.push_constants.size() != 1 ||
                vertex.push_constants[0].block != shader.interface.push_constants[0].block ||
                vertex.push_constants[0].size != sizeof(UiRasterParameters))
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            shader.interface.vertex_inputs = std::move(vertex.vertex_inputs);
            shader.interface.entry_points
                .insert(shader.interface.entry_points.end(), vertex.entry_points.begin(), vertex.entry_points.end());
            shader.interface.push_constants[0].stages |= vertex.push_constants[0].stages;
            auto program = context.graph.addProgram(std::move(shader));
            if (!program)
            {
                return cxx::unexpected(program.error());
            }
            RasterWork work;
            work.interface.program = *program;
            work.interface.parameters.values.resize(sizeof(UiRasterParameters));
            ResourceParameter images{"source_images", {context.resource(uiImagePort(0))}};
            images.descriptor_count = static_cast<std::uint32_t>(config.images.size());
            for (std::uint32_t i = 0; i < config.images.size(); ++i)
            {
                const auto resource = context.resource(uiImagePort(i));
                if (i != 0)
                {
                    images.elements.push_back({resource});
                }
                if (const auto* image = std::get_if<UiTextureImage>(&config.images[i]))
                {
                    context.arguments.push_back(StorageArgument::make(resource.resource, TextureBinding{image->texture})
                    );
                }
            }
            work.interface.resources.push_back(std::move(images));
            work.interface.samplers.push_back({"source_sampler", {context.addSampler({})}});
            work.graphics.vertex_bindings = {{0, sizeof(rdesc::UiVertex)}};
            work.graphics.vertex_attributes = {
                {0, 0, offsetof(rdesc::UiVertex, position), rdesc::ETextureFormat::RG32_SFLOAT},
                {1, 0, offsetof(rdesc::UiVertex, uv), rdesc::ETextureFormat::RG32_SFLOAT},
                {2, 0, offsetof(rdesc::UiVertex, color), rdesc::ETextureFormat::RGBA8_UNORM}
            };
            work.graphics.blend = {
                true,
                EBlendFactor::SOURCE_ALPHA,
                EBlendFactor::ONE_MINUS_SOURCE_ALPHA,
                EBlendFactor::ONE,
                EBlendFactor::ONE_MINUS_SOURCE_ALPHA
            };
            work.vertex_buffers = {{context.resource("vertices"), 0, 0}};
            work.draw = DrawIndexedList{
                context.resource("indices"),
                0,
                config.index_type,
                "source_index",
                static_cast<std::uint32_t>(config.images.size())
            };
            work.colors.push_back(
                {{context.resource("linear_color")}, EAttachmentLoad::CLEAR, EAttachmentStore::STORE, config.clear}
            );
            auto pass =
                context.graph.addRasterPass(featureKey(UiRasterFeatureDescriptor::name, key, "draw"), std::move(work));
            if (!pass)
            {
                return cxx::unexpected(pass.error());
            }
            if (auto published = context.publish("linear_color", pass->written.front()); !published)
            {
                return published;
            }
            auto output = rdesc::isSrgbTextureFormat(config.target.format)
                              ? detail::fullscreenRaster(context, kUiOutputLinear, kUiOutputLinearReflection)
                              : detail::fullscreenRaster(context, kUiOutputSrgb, kUiOutputSrgbReflection);
            if (!output)
            {
                return cxx::unexpected(output.error());
            }
            output->interface.resources = {{"linear_color", {pass->written.front()}}};
            output->colors.push_back({{context.resource("color")}});
            auto encoded = context.graph.addRasterPass(
                featureKey(UiRasterFeatureDescriptor::name, key, "output"),
                std::move(*output)
            );
            if (!encoded)
            {
                return cxx::unexpected(encoded.error());
            }
            return context.publish("color", encoded->written.front());
        }
    } // namespace

    RenderResult<std::size_t> ConfigValueTraits<UiRasterFeatureConfig>::retainedBytes(
        const UiRasterFeatureConfig& config
    ) noexcept
    {
        auto bytes = sizeof(config) + config.images.capacity() * sizeof(VUiImage);
        for (const auto& image : config.images)
        {
            if (const auto* texture = std::get_if<UiTextureImage>(&image))
            {
                const auto text = reinterpret_cast<std::uintptr_t>(texture->storage_key.data());
                const auto object = reinterpret_cast<std::uintptr_t>(&texture->storage_key);
                if (text < object || text >= object + sizeof(texture->storage_key))
                {
                    bytes += texture->storage_key.capacity() + 1;
                }
            }
        }
        return bytes;
    }

    const FeatureDescriptor UiRasterFeatureDescriptor::descriptor{
        name,
        cxx::typeToken<Config>(),
        "Frame-scoped UI geometry with clipping, linear alpha blending and one display encoding.",
        describeUi
    };

    const FeatureGraphOps& detail::UiRasterFeature::graph(bool) noexcept
    {
        static const FeatureGraphOps operations{buildUi};
        return operations;
    }
} // namespace lux::render
