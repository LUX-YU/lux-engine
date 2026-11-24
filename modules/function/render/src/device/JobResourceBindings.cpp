#include <lux/engine/render/device/JobGpuResources.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/target/TargetOutput.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/graph/execute/PhysicalFrameAllocation.hpp>

namespace lux::render::detail
{
    RenderResult<void> JobGpuResources::installFrame(
        InstalledGraphBindings& installed, std::uint32_t slot, std::span<const HistoryState> history_states, const TargetFrame* output
    ) noexcept
    {
        const auto& core = session.nriCore();
        const auto& plan = installed.graph;
        auto& frame = installed.frames[slot];
        frame.native.prepared_buffers.resize(plan.dynamicBuffers().size());
        frame.raster_draws.resize(plan.recordingDependencies().raster_list_count);
        frame.stream_descriptor_rows.resize(plan.programs().rasterStreams().size());
        auto allocation = frame.native.preparePhysical(session, {plan, installed.target, std::nullopt});
        if (!allocation) return cxx::unexpected(allocation.error());
        frame.memory_bindings = std::move(allocation->placements);
        for (auto& replacement : allocation->resources)
            frame.bindings[replacement.index] = std::move(replacement.binding);
        for (std::uint32_t i = 0; i < frame.bindings.size(); ++i)
        {
            auto& binding = frame.bindings[i];
            const auto& resource = plan.resources()[i];
            if (!resource.memory_used || resource.shared_table)
            {
                continue;
            }
            if (resource.history != kGraphInvalidIndex)
            {
                const auto& history = history_states[resource.history];
                const auto selected =
                    resource.origin == EGraphOrigin::HISTORY_PREVIOUS ? history.previous : history.current;
                const auto& physical = history.slots[selected];
                binding.select(physical.buffer, physical.image, core);
                binding.buffer_size = physical.buffer_size;
                continue;
            }
            if (resource.origin == EGraphOrigin::FRAME_BUFFER) continue;
            if (resource.origin == EGraphOrigin::IMPORTED)
            {
                if (std::holds_alternative<GraphImageDesc>(resource.description))
                {
                    if (!binding.image)
                    {
                        return cxx::unexpected(RenderError{kDeviceArgument});
                    }
                    binding.select(binding.buffer, binding.image, core);
                    if (auto valid = installed.qualifyImage({i}, *binding.image); !valid) return valid;
                }
                continue;
            }
            if (i == plan.output())
            {
                binding.select(nullptr, output->image, core);
                continue;
            }
            if (resource.layer_slot != kGraphInvalidIndex)
            {
                auto created = createGraphResource(
                    session, resource, installed.target, ETransientAllocation::INDEPENDENT
                );
                if (!created) return cxx::unexpected(created.error());
                binding = *created;
                frame.native.layered_images.push_back({GraphResourceId{i}, binding.image, {}});
                continue;
            }
        }
        for (auto& binding : frame.bindings) binding.select(binding.buffer, binding.image, core);
        if (auto passes = frame.prepareBindings(plan, session, output ? output->view : nullptr); !passes) return passes;
        frame.prepareAliasBarriers(plan.physicalResources());
        frame.command_batches.resize(plan.submissions().size());
        return frame.native.createBatches(session, plan.submissions());
    }

    void JobGpuResources::releaseOutputViews(InstalledGraphBindings& installed) noexcept
    {
        const auto resource = installed.graph.output();
        if (resource == kGraphInvalidIndex) return;
        const auto& uses = installed.graph.resources()[resource];
        std::vector<nri::Descriptor*> views;
        for (auto& frame : installed.frames)
        {
            frame.collectRasterViews(nullptr, frame.bindings[resource].image, views);
            frame.bindings[resource].select(nullptr, nullptr, session.nriCore());
            for (auto* output : frame.output_views)
            {
                *output = nullptr;
            }
            for (const auto index : uses.barrier_uses)
            {
                frame.entry_images[index].texture = nullptr;
            }
            for (auto index : uses.exit_barrier_uses)
            {
                frame.exit_images[index].texture = nullptr;
            }
            for (const auto [pass, index] : uses.descriptor_uses)
            {
                auto*& descriptor = frame.passes[pass].private_views[index];
                if (descriptor)
                {
                    views.push_back(std::exchange(descriptor, nullptr));
                }
            }
            frame.invalidateResource(installed.graph.recordingDependencies(), resource);
        }
        session.deferRelease(
            accepted,
            [views = std::move(views), core = &session.nriCore()]() noexcept
            {
                for (auto* view : views)
                {
                    core->DestroyDescriptor(view);
                }
            }
        );
    }
} // namespace lux::render::detail
