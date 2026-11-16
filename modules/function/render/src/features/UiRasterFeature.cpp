#include "UiOutputLinear.hpp"
#include "UiOutputSrgb.hpp"
#include "UiRasterFragment.hpp"
#include "UiRasterVertex.hpp"

#include <imgui.h>

static_assert(IMGUI_VERSION_NUM == 19188, "Render requires the pinned Lux ImGui fork ABI");
static_assert(sizeof(ImDrawVert) == 20 && offsetof(ImDrawVert, col) == 16);
static_assert(sizeof(ImDrawIdx) == 2 && sizeof(ImTextureID) == 8);
#include <lux/engine/render/features/BuiltinFeatures.hpp>
#include <lux/engine/render/features/FullscreenRaster.hpp>
#include <lux/engine/render/features/UiRasterFeature.hpp>

namespace lux::render
{
    namespace
    {
        RenderResult<void> describeUi(const void* input, std::string_view, FeaturePorts& ports) noexcept
        {
            const auto& config = *static_cast<const UiRasterFeatureConfig*>(input);
            if (auto target = validateEffectOutput(config.output); !target)
            {
                return target;
            }
            const bool invalid_geometry = config.output.samples != 1 || config.index_type > EIndexType::UINT32;
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
            // One shared descriptor index space; actual image identities arrive with each frame.
            ports.resources.push_back(
                {"sampled_images",
                 EFeaturePort::OUTPUT,
                 GraphImageDesc{1, 1, 1, 1, rdesc::ETextureFormat::RGBA8_UNORM},
                 EGraphOrigin::IMPORTED,
                 {GraphImageRange{1, 0, 1, 0, 1}}}
            );
            return {};
        }

        RenderResult<void> buildUi(
            const void* input,
            std::string_view key,
            FeatureBuildContext& context,
            const lux::render::detail::EnabledDeviceFeatures& enabled
        ) noexcept
        {
            const auto& config = *static_cast<const UiRasterFeatureConfig*>(input);
            auto vertex_program = kUiRasterVertex.load(enabled.shader_target);
            auto shader = kUiRasterFragment.load(enabled.shader_target);
            auto& vertex = vertex_program.interface;
            const bool valid_vertex = !vertex_program.stages.empty();
            const bool valid_fragment = !shader.stages.empty();
            shader.stages.insert(shader.stages.begin(), vertex_program.stages.begin(), vertex_program.stages.end());
            if (!valid_vertex || !valid_fragment || vertex.push_constants.size() != 1 ||
                shader.interface.push_constants.size() != 1 ||
                vertex.push_constants[0].block != shader.interface.push_constants[0].block ||
                !UiDrawParameters::matchesLayout(vertex.push_constants[0].block))
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
            auto& fixed_work = work.fixed();
            fixed_work.interface.program = *program;
            fixed_work.interface.parameters.values.resize(vertex.push_constants[0].size);
            fixed_work.interface.resources.push_back({"source_images", {context.resource("sampled_images")}});
            fixed_work.interface.samplers.push_back({"source_sampler", {context.addSampler({})}});
            fixed_work.graphics.vertex_bindings = {{0, sizeof(ImDrawVert)}};
            fixed_work.graphics.vertex_attributes = {
                {0, 0, offsetof(ImDrawVert, pos), rdesc::ETextureFormat::RG32_SFLOAT},
                {1, 0, offsetof(ImDrawVert, uv), rdesc::ETextureFormat::RG32_SFLOAT},
                {2, 0, offsetof(ImDrawVert, col), rdesc::ETextureFormat::RGBA8_UNORM}
            };
            fixed_work.graphics.blend = {
                true,
                EBlendFactor::SOURCE_ALPHA,
                EBlendFactor::ONE_MINUS_SOURCE_ALPHA,
                EBlendFactor::ONE,
                EBlendFactor::ONE_MINUS_SOURCE_ALPHA
            };
            fixed_work.vertex_buffers = {{context.resource("vertices"), 0, 0}};
            fixed_work.draw = DrawIndexedList{context.resource("indices"), 0, config.index_type, "source_index"};
            work.colors.push_back(
                {{context.resource("linear_color")}, EAttachmentLoad::CLEAR, EAttachmentStore::STORE, config.clear}
            );
            auto pass =
                context.graph.addRasterPass(featureKey(detail::UiRasterFeature::name, key, "draw"), std::move(work));
            if (!pass)
            {
                return cxx::unexpected(pass.error());
            }
            if (auto published = context.publish("linear_color", pass->written.front()); !published)
            {
                return published;
            }
            auto output = rdesc::isSrgbTextureFormat(config.output.format)
                              ? detail::fullscreenRaster(context, kUiOutputLinear.load(enabled.shader_target))
                              : detail::fullscreenRaster(context, kUiOutputSrgb.load(enabled.shader_target));
            if (!output)
            {
                return cxx::unexpected(output.error());
            }
            output->fixed().interface.resources = {{"linear_color", {pass->written.front()}}};
            output->colors.push_back({{context.resource("color")}});
            auto encoded = context.graph.addRasterPass(
                featureKey(detail::UiRasterFeature::name, key, "output"),
                std::move(*output)
            );
            if (!encoded)
            {
                return cxx::unexpected(encoded.error());
            }
            return context.publish("color", encoded->written.front());
        }
    } // namespace

    RenderResult<std::size_t>
    ConfigValueTraits<UiRasterFeatureConfig>::retainedBytes(const UiRasterFeatureConfig& config) noexcept
    {
        return sizeof(config);
    }

    const FeatureGraphOps& detail::UiRasterFeature::graph(bool) noexcept
    {
        static const FeatureGraphOps operations{describeUi, buildUi};
        return operations;
    }
} // namespace lux::render
