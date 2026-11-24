#include "HzbDownsample.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <lux/engine/render/assembly/BuiltinEffects.hpp>
#include <lux/engine/render/effect/capabilities/DepthReduction.hpp>
#include <lux/engine/render/effect/implementations/common/EffectArguments.hpp>
#include <lux/engine/render/effect/EffectCompileContext.hpp>
#include <lux/engine/render/effect/EffectImplementation.hpp>

namespace lux::render
{
    namespace
    {
        class DepthPyramid final : public EffectImplementation
        {
        public:
            explicit DepthPyramid(std::uint32_t count) noexcept : EffectImplementation(sizeof(count)), mip_count_(count) {}
            RenderResult<void> onBuild(EffectCompileContext&) const noexcept override;

        private:
            std::uint32_t mip_count_;
        };

        EffectCreateResult createDepthPyramid(graph::NodeId node, std::string_view argument) noexcept
        {
            constexpr auto mips = cxx::args::Option<std::uint32_t, "mips">{}
                .def(1u).check(cxx::args::inRange(1u, std::uint32_t{std::numeric_limits<std::uint32_t>::digits}));
            const auto parsed = cxx::args::parse(argument, mips);
            if (!parsed) return cxx::unexpected(detail::effectArgumentError(node, parsed.error()));
            return std::make_unique<DepthPyramid>(*parsed.get<"mips">());
        }

        RenderResult<void> DepthPyramid::onBuild(EffectCompileContext& context) const noexcept
        {
            const auto mip_count = mip_count_;
            const auto input = context.inputs(DepthReductionPorts::depth.semantic);
            if (input.size() != 1 || input.front().size() != 1)
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {context.nodeId().value}});
            const auto depth_version = input.front().front().version;
            const auto* description = context.resourceDescription(depth_version.resource);
            const auto* depth = description ? std::get_if<GraphImageDesc>(description) : nullptr;
            if (!depth) return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {context.nodeId().value}});
            const bool invalid_format = depth->format != rdesc::ETextureFormat::D32_SFLOAT;
            const bool invalid_layers = depth->layers != 1 || depth->runtime_layers || depth->samples != 1;
            const bool invalid_mips = depth->extent.source == EImageExtentSource::FIXED &&
                mip_count > std::bit_width(std::max(depth->width, depth->height));
            if (invalid_format || invalid_layers || invalid_mips)
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {context.nodeId().value}});

            auto pyramid = *depth;
            pyramid.format = rdesc::ETextureFormat::R32_SFLOAT;
            pyramid.mips = mip_count;
            const std::string key = "depth-pyramid/" + std::to_string(context.nodeId().value);
            const bool needs_history = context.outputDemanded(DepthReductionPorts::previous.semantic);
            ResourceVersion current, previous;
            if (needs_history)
            {
                auto history = context.declareHistory(key, {pyramid, {{1, 1, 1, 1}}, EGraphUse::SAMPLED});
                if (!history) return cxx::unexpected(history.error());
                previous = {history->previous, 0};
                current = {history->current, 0};
            }
            else
            {
                auto resource = context.declareResource({key, pyramid});
                if (!resource) return cxx::unexpected(resource.error());
                current = {*resource, 0};
            }
            const auto program = context.addProgram(GraphProgram{kHzbDownsample});
            if (!program) return cxx::unexpected(program.error());
            const auto first = context.nextWriteVersion(current.resource);
            if (!first) return cxx::unexpected(first.error());
            std::vector<ComputeWork> commands;
            commands.reserve(mip_count);
            for (std::uint32_t mip = 0; mip < mip_count; ++mip)
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
            auto pass = context.addComputeSequence(key, std::move(commands));
            if (!pass) return cxx::unexpected(pass.error());
            current = pass->written.front();
            if (needs_history)
            {
                if (auto recorded = context.publishHistory(current); !recorded) return recorded;
                if (auto exposed = context.publish(DepthReductionPorts::previous.semantic, {{DepthReductionPorts::previous.semantic, previous}});
                    !exposed)
                    return exposed;
            }
            if (context.outputDemanded(DepthReductionPorts::current.semantic))
                return context.publish(DepthReductionPorts::current.semantic, {{DepthReductionPorts::current.semantic, current}});
            return {};
        }
    }

    ImplementationEntry detail::depthPyramidImplementation() noexcept
    {
        ImplementationEntry implementation;
        implementation.capability = capabilities::DepthReduction.id;
        implementation.identity = {"lux.render.depth_pyramid.forward_zo", capabilities::DepthReduction.version};
        implementation.supported_outputs = {DepthReductionPorts::current.semantic, DepthReductionPorts::previous.semantic};
        implementation.create = createDepthPyramid;
        return implementation;
    }
} // namespace lux::render
