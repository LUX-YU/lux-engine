#include <lux/engine/render/frame/FrameError.hpp>
#include <lux/engine/render/runtime/RasterProgramInputs.hpp>
#include <lux/engine/render/runtime/commands/Error.hpp>
#include <lux/engine/render/device/JobGpuResources.hpp>
#include <lux/engine/render/runtime/DeviceEntry.hpp>
#include <lux/engine/render/device/RasterProgramUpdate.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/graph/execute/RasterStreamPlan.hpp>
#include <algorithm>

namespace lux::render::detail
{
    RasterProgramInputs::~RasterProgramInputs() noexcept
    {
        if (!device_) return;
        for (auto& use : subscriptions_) use.source->detach(use);
        if (unready_) device_->retireProgramBinding(binding_);
    }

    RenderResult<void> RasterProgramInputs::bind(
        const ResourceRegistry& registry,
        std::span<const SnapshotSource> snapshots, std::span<const BoundDrawResource> draw_resources
    ) noexcept
    {
        const auto& plan = graph_.cpuProgram();
        const bool wrong_inputs = snapshots.size() != plan.snapshots.size() ||
            draw_resources.size() != plan.draw_resource_bindings.size();
        if (registry_ || wrong_inputs)
            return cxx::unexpected(RenderError{kStorageContract});
        registry_ = &registry;
        builds_.reserve(plan.raster_builds.size());
        unready_ = static_cast<std::uint32_t>(plan.raster_builds.size());
        for (const auto& recipe : plan.raster_builds)
        {
            const auto slot = static_cast<std::uint32_t>(builds_.size());
            auto& build = builds_.emplace_back();
            build.compile = recipe.build;
            build.inputs = plan.calls[recipe.input_owner].call.inputs.data();
            for (const auto& stream : plan.streamsFor(plan.calls[recipe.input_owner]))
            {
                if (!graph_.programs().rasterStream(stream.pass)->runtime) continue;
                auto& selected = build.streams.emplace_back(stream.pass, stream.list);
                const auto range = plan.draw_resource_ranges[stream.pass];
                for (const auto& grant : draw_resources.subspan(range.first, range.count))
                    selected.grants.push_back(grant.id);
            }
            for (const auto source_slot : std::span{plan.program_reads}.subspan(recipe.snapshots.first, recipe.snapshots.count))
            {
                const auto& binding = snapshots[source_slot];
                const auto expected = plan.snapshots[source_slot].type;
                const bool invalid = !binding.storage || !binding.ops || !binding.ops->program_input;
                if (invalid || binding.ops->type != expected || binding.ops->type.name() != expected.name())
                    return cxx::unexpected(RenderError{kStorageMismatch, {source_slot}});
                const auto existing = std::ranges::find_if(sources_, [&](const auto& entry)
                {
                    return entry.binding.id == binding.id;
                });
                const auto source = static_cast<std::uint32_t>(existing - sources_.begin());
                if (existing == sources_.end())
                    sources_.push_back({binding, &binding.ops->program_input(binding.storage)});
                else if (existing->binding.ops != binding.ops || existing->binding.storage != binding.storage)
                    return cxx::unexpected(RenderError{kStorageMismatch, {source_slot}});
                const auto found = std::ranges::find(build.sources, source);
                const auto local = static_cast<std::uint32_t>(found - build.sources.begin());
                build.reads.push_back(local);
                if (found != build.sources.end()) continue;
                build.sources.push_back(source);

                auto* revision = sources_[source].revision;
                const auto use = std::ranges::find(subscriptions_, revision, &ProgramInputRevision::Use::source);
                if (use == subscriptions_.end()) subscriptions_.push_back({this, revision, {slot}});
                else if (use->builds.back() != slot) use->builds.push_back(slot);
            }
        }
        return {};
    }

    void RasterProgramInputs::install(DeviceEntry& device, ExecutionBindingId binding) noexcept
    {
        if (device_ || !binding.generation) std::terminate();
        device_ = &device;
        binding_ = binding;
        for (auto& use : subscriptions_) use.source->attach(use);
        if (unready_) device.activateProgramBinding(binding);
    }

    void RasterProgramInputs::invalidate(std::span<const std::uint32_t> builds) noexcept
    {
        if (!unready_) device_->activateProgramBinding(binding_);
        for (const auto slot : builds)
        {
            auto& build = builds_[slot];
            if (build.revision == UINT64_MAX) std::terminate();
            ++build.revision;
            if (build.state == Build::EState::READY) ++unready_;
            if (build.state == Build::EState::FAILED) --failed_;
            build.state = Build::EState::DIRTY;
            build.error = {};
        }
    }

    std::uint32_t RasterProgramInputs::nextBuild() const noexcept
    {
        if (unready_ == failed_) return kGraphInvalidIndex;
        for (std::uint32_t slot = 0; slot < builds_.size(); ++slot)
            if (builds_[slot].state == Build::EState::DIRTY && !inputsBusy(slot)) return slot;
        return kGraphInvalidIndex;
    }

    bool RasterProgramInputs::inputsBusy(std::uint32_t slot) const noexcept
    {
        const auto& build = builds_[slot];
        for (const auto source : build.sources)
            if (registry_->hasExclusiveCommand(sources_[source].binding.id)) return true;
        for (const auto& stream : build.streams)
            for (const auto id : stream.grants)
                if (!id.isNull() && registry_->hasExclusiveCommand(id)) return true;
        return false;
    }

    RenderError RasterProgramInputs::reportableError() const noexcept
    {
        if (!failed_) return {};
        for (std::uint32_t slot = 0; slot < builds_.size(); ++slot)
            if (builds_[slot].state == Build::EState::FAILED && !inputsBusy(slot)) return builds_[slot].error;
        return {};
    }

    const ProgramInputRevision* RasterProgramInputs::revisionSource(ResourceInstanceId id) const noexcept
    {
        const auto found = std::ranges::find_if(sources_, [&](const auto& source) { return source.binding.id == id; });
        return found == sources_.end() ? nullptr : found->revision;
    }

    const void* RasterProgramInputs::mapping(std::uint32_t slot) const noexcept
    {
        return slot == kGraphInvalidIndex ? nullptr : builds_[slot].mapping.get();
    }

    bool RasterProgramInputs::current(const RasterProgramUpdate& update) const noexcept
    {
        return update.build < builds_.size() && builds_[update.build].revision == update.input_revision;
    }

    RenderResult<void> RasterProgramInputs::select(RasterProgramUpdate& update) const noexcept
    {
        const auto& build = builds_[update.build];
        update.input_revision = build.revision;
        RasterProgramBuildData data{*registry_, gpu_.session};
        for (const auto& stream : build.streams)
        {
            auto& selection = data.streams.emplace_back(stream.pass);
            selection.sources = stream.grants;
            selection.ready = gpu_.dynamicRasterPrograms(stream.list) != nullptr;
        }

        data.snapshot_reads = build.reads;
        data.snapshots.reserve(build.sources.size());
        for (const auto slot : build.sources)
        {
            const auto& source = sources_[slot];
            auto selected = source.binding.ops->select(source.binding.storage);
            if (!selected) return cxx::unexpected(selected.error());
            if (!selected->value || selected->revision != source.revision->value())
                return cxx::unexpected(RenderError{kStorageContract, {slot}});
            data.snapshots.push_back({source.binding.id, *selected});
        }
        RasterProgramBuildContext context{data};
        auto mapping = build.compile(build.inputs, context, build.mapping.get());
        if (!mapping) return cxx::unexpected(mapping.error());
        update.mapping = std::move(*mapping);
        for (const auto& entry : data.streams)
        {
            if (entry.reuse) continue;
            if (!entry.programs) return cxx::unexpected(RenderError{kFrameContract, {entry.pass}});
            const auto& compiled = *graph_.programs().rasterStream(entry.pass);
            const auto& contract = *compiled.runtime;
            auto& stream = update.streams.emplace_back();
            stream.pass = entry.pass;
            stream.candidate = std::make_unique<DynamicRasterPrograms>();
            const auto* previous = gpu_.dynamicRasterPrograms(compiled.list);
            if (previous && previous->revision == UINT64_MAX) return cxx::unexpected(RenderError{kDeviceCapacity});
            stream.candidate->revision = previous ? previous->revision + 1 : 1;
            stream.candidate->programs.reserve(entry.programs->size());
            stream.programs.reserve(entry.programs->size());
            for (const auto& program : *entry.programs)
            {
                auto link = linkRasterStreamProgram(
                    gpu_.session, {contract.arguments, program.parameters, contract.target,
                        entry.shaders[program.shader.index], program.graphics, program.geometry_locations}
                );
                if (!link) return cxx::unexpected(link.error());
                stream.programs.push_back({std::move(*link), program.source});
            }
        }
        return {};
    }

    void RasterProgramInputs::settle(RasterProgramUpdate& update, std::span<FrameResources> frames) noexcept
    {
        auto& build = builds_[update.build];
        if (!current(update) || build.state != Build::EState::DIRTY || !update.returned) std::terminate();
        if (update.returned->type)
        {
            build.error = *update.returned;
            build.state = Build::EState::FAILED;
            ++failed_;
            return;
        }
        for (auto& stream : update.streams)
        {
            // The whole build succeeded, still matches its installed inputs, and can be adopted on this owner.
            const auto& ready = stream.candidate->programs;
            for (std::size_t i = 0; i < ready.size(); ++i)
                stream.programs[i].source.retainAdopted(*registry_, gpu_.session, ready[i].cache_id);
            const auto list = graph_.programs().rasterStream(stream.pass)->list;
            const auto* previous = gpu_.dynamicRasterPrograms(list);
            if (previous && previous->programs == stream.candidate->programs) continue;
            if (previous && !previous->programs.empty())
            {
                std::vector<nri::Descriptor*> views;
                for (auto& frame : frames)
                {
                    frame.invalidatePass(graph_, stream.pass);
                    frame.releaseRasterBindings(list, views);
                }
                if (!views.empty()) gpu_.session.deferRelease(gpu_.accepted,
                    [core = &gpu_.session.nriCore(), views = std::move(views)]() noexcept
                    {
                        for (auto* view : views) core->DestroyDescriptor(view);
                    }
                );
            }
            gpu_.adoptRasterPrograms(list, std::move(stream.candidate));
        }
        build.mapping.swap(update.mapping);
        build.state = Build::EState::READY;
        if (!--unready_) device_->retireProgramBinding(binding_);
    }
} // namespace lux::render::detail
