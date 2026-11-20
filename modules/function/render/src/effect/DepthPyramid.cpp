#include "HzbDownsample.hpp"
#include <algorithm>
#include <bit>
#include <charconv>
#include <limits>
#include <lux/engine/render/effect/BuiltinEffectNodes.hpp>
#include <lux/engine/render/effect/DepthPyramid.hpp>
#include <lux/engine/render/extensions/effect/EffectBuildContext.hpp>
#include <lux/engine/render/extensions/effect/EffectImplementation.hpp>
#include <lux/engine/render/extensions/effect/EffectInitializationContext.hpp>
#include <lux/engine/render/extensions/graph/RenderGraphBuilder.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>

namespace lux::render
{
    namespace
    {
        RenderResult<std::uint32_t> parseMipCount(graph::NodeId node, std::string_view argument) noexcept
        {
            if (argument.empty()) return 1;
            std::uint32_t count = 0;
            const auto end = argument.data() + argument.size();
            const auto parsed = std::from_chars(argument.data(), end, count);
            const bool invalid_count = count == 0 || count > std::numeric_limits<std::uint32_t>::digits;
            if (parsed.ec != std::errc{} || parsed.ptr != end || invalid_count)
                return cxx::unexpected(RenderError{kInvalidConfig, {node.value}});
            return count;
        }

        RenderResult<void>
        initializeDepthPyramid(EffectInitializationContext& context, std::string_view argument) noexcept
        {
            const auto count = parseMipCount(context.nodeId(), argument);
            if (!count) return cxx::unexpected(count.error());
            return {};
        }

        RenderResult<void> buildDepthPyramid(EffectBuildContext& context, std::string_view argument) noexcept
        {
            const auto mip_count = parseMipCount(context.nodeId(), argument);
            if (!mip_count) return cxx::unexpected(mip_count.error());
            const auto input = context.inputs(DepthPyramid::depth);
            if (input.size() != 1 || input.front().size() != 1)
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {context.nodeId().value}});
            const auto depth_version = input.front().front().version;
            auto& builder = context.builder();
            const auto* description = builder.resourceDescription(depth_version.resource);
            const auto* depth = description ? std::get_if<GraphImageDesc>(description) : nullptr;
            if (!depth) return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {context.nodeId().value}});
            const bool invalid_format = depth->format != rdesc::ETextureFormat::D32_SFLOAT;
            const bool invalid_layers = depth->layers != 1 || depth->runtime_layers || depth->samples != 1;
            const bool invalid_mips = depth->extent.source == EImageExtentSource::FIXED &&
                *mip_count > std::bit_width(std::max(depth->width, depth->height));
            if (invalid_format || invalid_layers || invalid_mips)
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {context.nodeId().value}});

            auto pyramid = *depth;
            pyramid.format = rdesc::ETextureFormat::R32_SFLOAT;
            pyramid.mips = *mip_count;
            const std::string key = "depth-pyramid/" + std::to_string(context.nodeId().value);
            const bool needs_history = context.outputDemanded(DepthPyramid::previous);
            ResourceVersion current, previous;
            if (needs_history)
            {
                auto history = builder.declareHistory(key, pyramid, {{1, 1, 1, 1}}, EGraphUse::SAMPLED);
                if (!history) return cxx::unexpected(history.error());
                previous = {history->previous, 0};
                current = {history->current, 0};
            }
            else
            {
                auto resource = builder.declareResource({key, pyramid});
                if (!resource) return cxx::unexpected(resource.error());
                current = {*resource, 0};
            }
            const auto program = builder.addProgram(GraphProgram{kHzbDownsample});
            if (!program) return cxx::unexpected(program.error());
            const auto first = builder.nextWriteVersion(current.resource);
            if (!first) return cxx::unexpected(first.error());
            std::vector<ComputeWork> commands;
            commands.reserve(*mip_count);
            for (std::uint32_t mip = 0; mip < *mip_count; ++mip)
            {
                const auto source = mip ? current : depth_version;
                const GraphImageRange read{mip ? 1u : 2u, mip ? mip - 1 : 0, 1, 0, 1};
                const GraphImageRange write{1, mip, 1, 0, 1};
                ComputeWork work;
                work.interface.program = *program;
                work.interface.resources = {
                    {"uSrc", {source, read, EGraphAccess::READ}},
                    {"uDst", {current, write, EGraphAccess::WRITE}}
                };
                work.dispatch = ImageDispatchArguments{current.resource, mip};
                commands.push_back(std::move(work));
                current.number = first->number + mip;
            }
            auto pass = builder.addComputeSequence(key, std::move(commands));
            if (!pass) return cxx::unexpected(pass.error());
            current = pass->written.front();
            if (needs_history)
            {
                if (auto recorded = builder.publishHistory(current); !recorded) return recorded;
                if (auto exposed = context.publish(DepthPyramid::previous, {{DepthPyramid::previous, previous}});
                    !exposed)
                    return exposed;
            }
            if (context.outputDemanded(DepthPyramid::current))
                return context.publish(DepthPyramid::current, {{DepthPyramid::current, current}});
            return {};
        }
    }

    CapabilityDefinition DepthPyramid::definition() noexcept
    {
        CapabilityDefinition definition;
        definition.identity = {graph::nodeTypeId(capability), std::string{capability}, version};
        definition.display_name = "Depth Pyramid";
        definition.ports = {
            {depth, "depth", {std::string{depth_type}, 1}, graph::EPinDirection::INPUT, 1, true},
            {current, "current", {std::string{pyramid_type}, 1}, graph::EPinDirection::OUTPUT},
            {previous, "previous", {std::string{pyramid_type}, 1}, graph::EPinDirection::OUTPUT}
        };
        return definition;
    }

    RenderResult<void> detail::registerDepthPyramid(RenderServer& server) noexcept
    {
        auto definition = DepthPyramid::definition();
        const auto capability = definition.identity.id;
        if (auto registered = server.registerEffectCapability(std::move(definition)); !registered) return registered;
        EffectImplementation implementation;
        implementation.capability = capability;
        implementation.identity = {std::string{DepthPyramid::forward_zo}, DepthPyramid::version};
        implementation.supported_outputs = {DepthPyramid::current, DepthPyramid::previous};
        implementation.initialize = initializeDepthPyramid;
        implementation.build = buildDepthPyramid;
        return server.registerEffectImplementation(std::move(implementation));
    }
} // namespace lux::render
