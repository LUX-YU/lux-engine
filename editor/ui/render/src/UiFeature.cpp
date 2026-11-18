#include "UiOutputLinear.hpp"
#include "UiOutputSrgb.hpp"
#include "UiRasterFragment.hpp"
#include "UiRasterVertex.hpp"

#include "FullscreenVertex.hpp"
#include <lux/editor/ui/render/UiFeature.hpp>
#include <lux/editor/ui/render/UiState.hpp>

namespace lux::editor::ui
{
    static RenderResult<RasterWork> fullscreenRaster(FeatureBuildContext& context, GraphProgram shaders) noexcept
    {
        if (shaders.targets.empty())
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        auto parameter_bytes = UINT32_MAX;
        for (auto& shader : shaders.targets)
        {
            auto vertex = kFullscreenVertex.load(shader.target);
            if (shader.stages.empty() || vertex.stages.empty())
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            shader.stages.insert(shader.stages.begin(), vertex.stages.begin(), vertex.stages.end());
            shader.interface.entry_points.insert(
                shader.interface.entry_points.end(), vertex.interface.entry_points.begin(),
                vertex.interface.entry_points.end()
            );
            shader.interface.vertex_inputs = std::move(vertex.interface.vertex_inputs);
            const auto bytes =
                shader.interface.push_constants.empty() ? 0 : shader.interface.push_constants.front().size;
            parameter_bytes = std::min(parameter_bytes, bytes);
        }
        RasterWork work;
        auto& fixed_work = work.fixed();
        // Authors may fill the common byte prefix. Target-specific trailing padding is supplied by Compile.
        fixed_work.interface.parameters.values.resize(parameter_bytes);
        fixed_work.interface.parameters.zero_fill_tail = true;
        auto program = context.graph.addProgram(std::move(shaders));
        if (!program)
        {
            return cxx::unexpected(program.error());
        }
        fixed_work.interface.program = *program;
        return work;
    }

    RenderResult<void>
    UiRasterFeature::Ports::describe(const Config* input, std::string_view, FeaturePorts& ports) noexcept
    {
        const auto& config = *static_cast<const UiRasterFeatureConfig*>(input);
        if (auto target = validateEffectOutput(config.output); !target)
        {
            return target;
        }
        const bool invalid_geometry = config.output.samples != 1 || !config.draw.valid();
        const bool invalid_output = config.output.format != rdesc::ETextureFormat::RGBA8_UNORM &&
                                    config.output.format != rdesc::ETextureFormat::RGBA8_SRGB &&
                                    config.output.format != rdesc::ETextureFormat::BGRA8_UNORM &&
                                    config.output.format != rdesc::ETextureFormat::BGRA8_SRGB;
        if (invalid_geometry || invalid_output)
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        ports.resources.push_back(
            {"vertices",
             EFeaturePort::OUTPUT,
             GraphBufferDesc{sizeof(ImDrawVert)},
             EGraphOrigin::IMPORTED,
             {GraphBufferRange{0, sizeof(ImDrawVert)}}}
        );
        ports.resources.push_back(
            {"indices", EFeaturePort::OUTPUT, GraphBufferDesc{4}, EGraphOrigin::IMPORTED, {GraphBufferRange{0, 4}}}
        );
        GraphImageDesc color{0, 0, 1, 1, rdesc::ETextureFormat::RGBA16_SFLOAT};
        color.extent.source = EImageExtentSource::TARGET;
        ports.resources.push_back({"linear_color", EFeaturePort::OUTPUT, color});
        color.format = config.output.format;
        ports.resources.push_back({"color", EFeaturePort::OUTPUT, color});
        ports.storage.view.push_back(
            {"snapshot",
             EFeaturePort::OUTPUT,
             GraphBufferDesc{4},
             EGraphOrigin::IMPORTED,
             {GraphBufferRange{0, 4}},
             ResourceRequirement{ResourceName(UiResource::descriptor.type.name())}}
        );
        ports.storage.global.push_back(
            {"textures",
             EFeaturePort::OUTPUT,
             GraphImageDesc{1, 1, 1, 1, rdesc::ETextureFormat::RGBA8_UNORM},
             EGraphOrigin::IMPORTED,
             {GraphImageRange{1, 0, 1, 0, 1}},
             ResourceRequirement{ResourceName(TextureResource::descriptor.type.name())}}
        );
        return {};
    }

    RenderResult<void>
    UiRasterFeature::build(const Config* input, std::string_view key, FeatureBuildContext& context) noexcept
    {
        const auto& config = *static_cast<const UiRasterFeatureConfig*>(input);
        GraphProgram shaders{kUiRasterFragment};
        for (auto& shader : shaders.targets)
        {
            auto vertex_program = kUiRasterVertex.load(shader.target);
            auto& vertex = vertex_program.interface;
            const bool valid_vertex = !vertex_program.stages.empty();
            const bool valid_fragment = !shader.stages.empty();
            shader.stages.insert(shader.stages.begin(), vertex_program.stages.begin(), vertex_program.stages.end());
            const bool has_parameter_blocks =
                vertex.push_constants.size() == 1 && shader.interface.push_constants.size() == 1;
            const bool has_matching_parameters =
                has_parameter_blocks && vertex.push_constants[0].block == shader.interface.push_constants[0].block &&
                UiDrawParameters::matchesLayout(vertex.push_constants[0].block);
            const bool is_invalid_program = !valid_vertex || !valid_fragment || !has_matching_parameters;
            if (is_invalid_program)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            shader.interface.vertex_inputs = std::move(vertex.vertex_inputs);
            shader.interface.entry_points.insert(
                shader.interface.entry_points.end(), vertex.entry_points.begin(), vertex.entry_points.end()
            );
            shader.interface.push_constants[0].stages |= vertex.push_constants[0].stages;
        }
        auto program = context.graph.addProgram(std::move(shaders));
        if (!program)
        {
            return cxx::unexpected(program.error());
        }
        RasterWork work;
        auto& fixed_work = work.fixed();
        fixed_work.interface.program = *program;
        fixed_work.interface.parameters.values.resize(sizeof(UiDrawParameters));
        fixed_work.interface.parameters.zero_fill_tail = true;
        fixed_work.interface.resources.push_back({"source_images", {context.resource("textures")}});
        fixed_work.interface.samplers.push_back({"source_sampler", {context.graph.addSampler({})}});
        fixed_work.graphics.vertex_bindings = {{0, sizeof(ImDrawVert)}};
        fixed_work.graphics.vertex_attributes = {
            {0, 0, offsetof(ImDrawVert, pos), rdesc::ETextureFormat::RG32_SFLOAT},
            {1, 0, offsetof(ImDrawVert, uv), rdesc::ETextureFormat::RG32_SFLOAT},
            {2, 0, offsetof(ImDrawVert, col), rdesc::ETextureFormat::RGBA8_UNORM}
        };
        fixed_work.graphics.blend = {
            true, EBlendFactor::SOURCE_ALPHA, EBlendFactor::ONE_MINUS_SOURCE_ALPHA, EBlendFactor::ONE,
            EBlendFactor::ONE_MINUS_SOURCE_ALPHA
        };
        fixed_work.vertex_buffers = {{context.resource("vertices"), 0, 0}};
        fixed_work.draw = DrawIndexedList{context.resource("indices"), 0, config.draw.index_type, "source_index"};
        work.colors.push_back(
            {{context.resource("linear_color")}, EAttachmentLoad::CLEAR, EAttachmentStore::STORE, config.draw.clear}
        );
        auto pass = context.graph.addRasterPass(featureKey(UiRasterFeature::name, key, "draw"), std::move(work));
        if (!pass)
        {
            return cxx::unexpected(pass.error());
        }
        const auto source = context.resource("snapshot").resource;
        context.require(StorageArgument::make(source, UiBinding{}));
        context.require(StorageArgument::make(context.resource("textures").resource, TextureBinding{}));
        auto recipe = ConfigValue::make(UiPreparation{source, pass->pass});
        if (!recipe)
        {
            return cxx::unexpected(recipe.error());
        }
        context.graph.addInstanceData({{}, std::move(*recipe), &UiInstance::create, {source}, 0, 0, {pass->pass}});
        if (auto published = context.publish("linear_color", pass->written.front()); !published)
        {
            return published;
        }
        auto output = rdesc::isSrgbTextureFormat(config.output.format)
                          ? fullscreenRaster(context, GraphProgram{kUiOutputLinear})
                          : fullscreenRaster(context, GraphProgram{kUiOutputSrgb});
        if (!output)
        {
            return cxx::unexpected(output.error());
        }
        output->fixed().interface.resources = {{"linear_color", {pass->written.front()}}};
        output->colors.push_back({{context.resource("color")}});
        auto encoded =
            context.graph.addRasterPass(featureKey(UiRasterFeature::name, key, "output"), std::move(*output));
        if (!encoded)
        {
            return cxx::unexpected(encoded.error());
        }
        return context.publish("color", encoded->written.front());
    }

} // namespace lux::editor::ui
