#include <lux/engine/render/features/HzbFeature.hpp>

#include "HzbDownsample.hpp"
#include <bit>
#include <lux/engine/render/features/BuiltinFeatures.hpp>

namespace lux::render
{
    namespace
    {
        RenderResult<void> describeHzb(const void* value, std::string_view, FeaturePorts& ports) noexcept
        {
            const auto& config = *static_cast<const HzbFeatureConfig*>(value);
            const auto& depth = config.depth;
            const bool invalid_extent = !depth.width || !depth.height || depth.layers != 1 || depth.samples != 1;
            const bool invalid_mips =
                !config.mip_count || config.mip_count > std::bit_width(std::max(depth.width, depth.height));
            const bool invalid_format = depth.format != rdesc::ETextureFormat::D32_SFLOAT;
            if (invalid_extent || invalid_mips || invalid_format)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            auto pyramid = depth;
            pyramid.format = rdesc::ETextureFormat::R32_SFLOAT;
            pyramid.mips = config.mip_count;
            ports.resources.push_back({"depth", EFeaturePort::INPUT, depth});
            ports.resources.push_back({"previous", EFeaturePort::OUTPUT, pyramid, EGraphOrigin::HISTORY_PREVIOUS});
            ports.resources.push_back({"current", EFeaturePort::OUTPUT, pyramid, EGraphOrigin::HISTORY_CURRENT});
            ports.histories.push_back({"previous", "current", {{1, 1, 1, 1}}, EGraphUse::SAMPLED});
            return {};
        }

        RenderResult<void> buildHzb(const void* value, std::string_view key, FeatureBuildContext& context) noexcept
        {
            const auto& config = *static_cast<const HzbFeatureConfig*>(value);
            rdesc::SpirvArtifact shader;
            shader.stages.push_back(
                {rdesc::EShaderType::COMPUTE, "main", {std::begin(kHzbDownsample), std::end(kHzbDownsample)}}
            );
            if (!rdesc::ShaderInfo::deserialize(kHzbDownsampleReflection, shader.interface))
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            const auto program = context.graph.addProgram(std::move(shader));
            if (!program)
            {
                return cxx::unexpected(program.error());
            }
            auto current = context.resource("current");
            for (std::uint32_t mip = 0; mip < config.mip_count; ++mip)
            {
                const auto source = mip ? current : context.resource("depth");
                const GraphImageRange source_range{mip ? 1u : 2u, mip ? mip - 1 : 0, 1, 0, 1};
                const GraphImageRange destination_range{1, mip, 1, 0, 1};
                ComputeWork work;
                work.interface.program = *program;
                work.interface.resources = {
                    {"uSrc", {source, source_range, EGraphAccess::READ}},
                    {"uDst", {current, destination_range, EGraphAccess::WRITE}}
                };
                work.dispatch = ImageDispatchArguments{current.resource, mip};
                auto pass = context.graph.addComputePass(
                    featureKey(detail::HzbFeature::name, key, "mip" + std::to_string(mip)),
                    std::move(work)
                );
                if (!pass)
                {
                    return cxx::unexpected(pass.error());
                }
                current = pass->written.front();
            }
            return context.publish("current", current);
        }
    } // namespace

    const FeatureGraphOps& detail::HzbFeature::graph(bool) noexcept
    {
        static const FeatureGraphOps graph{describeHzb, buildHzb};
        return graph;
    }
} // namespace lux::render
