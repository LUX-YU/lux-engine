#include <lux/engine/render/frame/FrameError.hpp>
#include <cstring>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/runtime/JobExecution.hpp>
#include <lux/engine/render/runtime/DeviceEntry.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>

namespace lux::render::runtime_detail
{
    namespace
    {
        std::vector<detail::FrameResources> createFrames(std::span<detail::JobFrameResources> physical, const compiled::RenderGraph& graph) noexcept
        {
            std::vector<detail::FrameResources> frames;
            frames.reserve(physical.size());
            for (auto& frame : physical) frames.emplace_back(frame).initializeRows(graph);
            return frames;
        }
    }

    std::uint32_t JobExecution::frameCount() const noexcept
    {
        return static_cast<std::uint32_t>(frame_bindings.frames.size());
    }

    detail::ResourceInput JobExecution::resourceInput(GraphResourceId resource, std::uint32_t slot) const noexcept
    {
        return frame_bindings.frames[slot].bindings[resource.index].input();
    }

    void JobExecution::bindImage(GraphResourceId resource, std::uint32_t slot, nri::Texture& image) noexcept
    {
        auto binding = resourceInput(resource, slot);
        if (binding.image == &image)
        {
            return;
        }
        binding.image = &image;
        frame_bindings.bind(resource, slot, binding);
    }

    void JobExecution::releaseOutputReferences() noexcept
    {
        output_.frames = {};
        gpu_.releaseOutputViews(frame_bindings);
    }

    std::uint32_t JobExecution::useFrameResource(detail::InstalledFrameResource binding) noexcept
    {
        if (!binding.ops || (!binding.ops->prepareFrame && !binding.ops->selectRun && !binding.ops->cpu_view))
            return kGraphInvalidIndex;
        const auto found = std::ranges::find(frame_resources, binding.id, &detail::InstalledFrameResource::id);
        const auto slot = static_cast<std::uint32_t>(found - frame_resources.begin());
        if (found == frame_resources.end()) frame_resources.push_back(binding);
        return slot;
    }

    ResourceSet& JobExecution::privateResources(ResourceSet& parent) noexcept
    {
        if (!gpu_.private_resources)
            gpu_.private_resources = std::make_unique<ResourceSet>(parent, EResourceDomain::JOB);
        return *gpu_.private_resources;
    }

    JobExecution::~JobExecution() noexcept
    {
        for (const auto& frame : frame_bindings.frames)
            for (const auto& batch : frame.command_batches)
                if (batch.recording.load(std::memory_order_acquire)) std::terminate();
    }

    JobExecution::JobExecution(
        detail::JobGpuResources& gpu,
        const compiled::RenderGraph& selected,
        Installation installation,
        detail::TargetOutput physical
    ) noexcept
        : effect(installation.effect), device(installation.device), target(installation.target),
          output_(physical), gpu_(gpu), condition_values_(selected.conditionCount()),
          frame_bindings{selected, gpu.session, output_.description, createFrames(gpu.frames, selected)},
          histories_(selected.histories().size()),
          binding_identity(installation.identity)
    {
    }

    RenderResult<void> JobExecution::bindFrameAlgorithms(std::span<const ResolvedResource> resources) noexcept
    {
        const auto& plan = device_graph->algorithms;
        cpu_draws_.resize(plan.draw_outputs.size());
        for (std::size_t i = 0; i < cpu_draws_.size(); ++i)
        {
            auto& destination = cpu_draws_[i];
            destination.job_ = this;
            destination.stream_ = plan.draw_outputs[i];
        }
        algorithm_workspaces_.reserve(plan.workspace_count);
        for (std::uint32_t i = 0; i < plan.calls.size(); ++i)
        {
            const auto& linked = plan.calls[i];
            if (const auto create = linked.call.methods->create_workspace)
            {
                const detail::FrameRasterOutputs draws{plan, draw_resource_ids, cpu_draws_, i};
                const InstalledDrawSources sources{draws, resources};
                auto workspace = create(linked.call.inputs.data(), targetDescription(), sources);
                if (!workspace) return cxx::unexpected(workspace.error());
                algorithm_workspaces_.push_back(std::move(*workspace));
            }
        }
        // Workspace installation has supplied all composed sources. Qualify every dynamic list once.
        const auto streams = graph().programs().rasterStreams();
        std::vector<bool> provided(program_bindings_.empty() ? 0 : streams.size());
        for (const auto& binding : program_bindings_)
        {
            const auto list = binding.stream->list;
            if (provided[list]) return cxx::unexpected(RenderError{kGraphAmbiguousProducer, {binding.stream->pass}});
            provided[list] = true;
        }
        for (const auto& stream : streams)
        {
            if (stream.runtime && (provided.empty() || !provided[stream.list]))
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {stream.pass}});
        }
        inline_bytes_.resize(plan.inline_bytes);
        return {};
    }

    RenderResult<void>
    JobExecution::selectPublishedPrograms(const extension_detail::SelectedResourceViews& views) noexcept
    {
        for (auto& binding : program_bindings_)
        {
            const auto& stream = *binding.stream;
            if (stream.resource_list && !passEnabledThisRun(stream.pass)) continue;
            const auto* view = binding.view == kGraphInvalidIndex ? nullptr :
                views.sources[views.slots[binding.view]].value;
            auto published = binding.read(view);
            if (!published) return cxx::unexpected(published.error());
            if (binding.revision && binding.revision == published->revision) continue;
            if (*stream.runtime != published->contract)
                return cxx::unexpected(RenderError{kStorageMismatch, {stream.pass}});
            auto selected = gpu_.bindProgramSlots(graph(), stream, frame_bindings.frames, published->programs);
            if (!selected) return cxx::unexpected(selected.error());
            binding.revision = published->revision;
        }
        return {};
    }

    RenderResult<void>
    JobExecution::selectFrameInputs(
        std::uint32_t slot,
        std::span<detail::PreparedBufferInput> outputs,
        const extension_detail::SelectedResourceViews& inputs,
        std::vector<detail::PreparedBufferUpload>& uploads
    ) noexcept
    {
        auto& destination = frame_bindings.frames[slot];
        auto buffer = frame_bindings.graph.preparedBufferCount();
        for (const auto extents : buffer_extents)
        {
            auto bound = destination.bindDynamicBuffer(frame_bindings.graph, gpu_.session, buffer++, extents[slot]);
            if (!bound) return bound;
        }
        if (auto ready = selectPublishedPrograms(inputs); !ready) return ready;
        if (device_graph && !device_graph->algorithms.empty())
        {
            if (auto prepared = runFrameAlgorithms(slot, outputs, inputs); !prepared) return prepared;
        }
        for (std::size_t i = 0; i < outputs.size(); ++i)
        {
            if (!frame_bindings.graph.dynamicBuffers()[i].used) continue;
            const auto& output = outputs[i];
            if (auto bound = destination.bindDynamicBuffer(graph(), gpu_.session, i, output.size); !bound) return bound;
            if (output.bytes.empty()) continue;
            auto& replica = destination.native.prepared_buffers[i];
            uploads.push_back({replica, gpu_.accepted, output.bytes, output.revision, replica.generation});
        }
        return {};
    }

    RenderResult<void>
    JobExecution::runFrameAlgorithms(
        std::uint32_t slot, std::span<detail::PreparedBufferInput> outputs, const extension_detail::SelectedResourceViews& inputs
    ) noexcept
    {
        const auto& algorithms = device_graph->algorithms;
        auto& destination = frame_bindings.frames[slot];
        if (!cpu_draws_.empty())
        {
            if (draw_selection_ == UINT64_MAX) return cxx::unexpected(RenderError{kDeviceCapacity});
            ++draw_selection_;
        }

        const auto& calls = algorithms.calls;
        for (std::size_t i = 0; i < calls.size(); ++i)
        {
            const auto& linked = calls[i];
            const auto streams = std::span{cpu_draws_}.subspan(linked.streams.first, linked.streams.count);
            for (auto& stream : streams)
            {
                stream.written = false;
                if constexpr (detail::kCollectReadMetrics)
                {
                    stream.textures.clear();
                    stream.records.clear();
                    stream.record_images.clear();
                }
                stream.frame_slot = slot;
            }
            const auto [first, count] = linked.buffers;
            const auto call_index = static_cast<std::uint32_t>(i);
            const detail::FrameRasterOutputs draws{algorithms, draw_resource_ids, cpu_draws_, call_index};
            EffectFrameWriter writer{
                draws, frame_bindings.graph.dynamicBuffers().subspan(first, count),
                std::span{destination.native.prepared_buffers}.subspan(first, count),
                outputs.subspan(first, count)
            };
            const extension_detail::SelectedResourceViews views{
                inputs.target, inputs.sources, inputs.slots.subspan(linked.first_view, linked.call.methods->inputs.size())
            };
            writer.inline_results_ = std::span{inline_bytes_}.subspan(linked.inline_offset, linked.inline_size);
            auto* workspace = linked.workspace_slot == kGraphInvalidIndex ?
                nullptr : algorithm_workspaces_[linked.workspace_slot].get();
            auto prepared = linked.call.methods->run(linked.call.inputs.data(), views, writer, workspace);
            for (auto& stream : streams) stream.frame_slot = kGraphInvalidIndex;
            if (!prepared) return prepared;

            if (count && !writer.published_)
            {
                const auto resource = frame_bindings.graph.dynamicBuffers()[first].resource;
                return cxx::unexpected(RenderError{kGraphUndefinedRead, {kGraphInvalidIndex, resource}});
            }
            for (const auto& storage : streams)
            {
                if (!storage.written) return cxx::unexpected(RenderError{kFrameContract, {storage.compiledStream().pass}});
            }
        }

        // Commit the complete selection only after every preparation callback has succeeded.
        for (const auto& copy : algorithms.constant_copies)
        {
            const auto input = std::span<const std::byte>{inline_bytes_}.subspan(copy.offset);
            std::uint64_t revision = 0;
            if (copy.revision_offset != kGraphInvalidIndex)
                std::memcpy(&revision, input.data() + copy.revision_offset, sizeof(revision));
            destination.applyInlineConstant(frame_bindings.graph, copy.pass, input.first(copy.bytes), revision);
        }
        return {};
    }
} // namespace lux::render::runtime_detail

namespace lux::render
{
    RenderResult<void>
    RenderServer::Impl::assembleExecution(
        JobExecution& candidate, ResourceRegistry::BindingChanges& installation, const JobBinding& binding,
        std::span<const ResolvedResource> resources
    ) noexcept
    {
        const auto target_images = std::span(binding.target_images);
        const auto& input = binding.inputs;
        auto& entry = *runtime_->device(candidate.device);
        candidate.target_images.reserve(target_images.size());
        const auto declared_targets = std::span{candidate.graph().targetImports()};
        if (std::ranges::adjacent_find(target_images, {}, &TargetImageInput::binding) != target_images.end())
            return cxx::unexpected(RenderError{kCommandContract});
        const bool matching_count = target_images.size() == declared_targets.size();
        for (std::size_t index = 0; index < target_images.size(); ++index)
        {
            const auto& input = target_images[index];
            if (input.read != EProductRead::CURRENT_RUN && input.read != EProductRead::LATEST_PUBLISHED)
                return cxx::unexpected(RenderError{kCommandContract, {index}});
            if (input.binding.empty()) return cxx::unexpected(RenderError{kCommandContract});
            const auto* source = runtime_->targets_.find(input.target);
            if (!source || !targetDescription(*source).sampled) return cxx::unexpected(RenderError{kRuntimeIdentity});
            if (!std::holds_alternative<detail::OffscreenTarget>(*source))
                return cxx::unexpected(RenderError{kDeviceUnsupported});
            if (targetState(*source).device != candidate.device) return cxx::unexpected(RenderError{kJobState});
            if (targetState(*source).pending_replacement.has_value()) return cxx::unexpected(RenderError{kDeviceBusy});
            if (!matching_count) continue;
            const auto& declaration = declared_targets[index];
            if (declaration.binding != input.binding) return cxx::unexpected(RenderError{kRuntimeIdentity, {index}});
            const auto images = targetOutput(*source);
            const bool matches = images.description.format == declaration.format &&
                images.description.samples == declaration.samples;
            if (!matches) return cxx::unexpected(RenderError{kDeviceArgument, {index}});
            const auto id = declaration.resource;
            if (id.index == kGraphInvalidIndex) continue;
            candidate.target_images.push_back({input.target, declaration, input.read});
            for (std::uint32_t slot = 0; slot < entry.frameProgress().frameCount(); ++slot)
            {
                candidate.bindImage(id, slot, *images.frames[slot].image);
            }
        }
        if (target_images.size() < declared_targets.size()) return cxx::unexpected(RenderError{kRuntimeIdentity});
        if (target_images.size() > declared_targets.size()) return cxx::unexpected(RenderError{kCommandContract});
        const auto& manifest = candidate.device_graph->resources;
        const auto& slots = manifest.operands;
        if (slots.size() != resources.size()) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
        const auto same_binding = [&](const compiled::BindingComparison& comparison) noexcept
        {
            const auto [left, right] = comparison;
            if (left.domain != right.domain) return false;
            if (left.domain == compiled::BindingOperand::EDomain::TARGET)
                return target_images[left.slot].target == target_images[right.slot].target &&
                    target_images[left.slot].read == target_images[right.slot].read;
            const auto& a = resources[left.slot];
            const auto& b = resources[right.slot];
            if (a.id.isNull() || b.id.isNull()) return false;
            const bool same_version = a.publication == b.publication ||
                (a.publication && b.publication && *a.publication == *b.publication);
            return same_version && a.id == b.id && slots[left.slot].projection == slots[right.slot].projection;
        };
        std::vector<std::uint8_t> selected;
        selected.reserve(candidate.graph().bindingComparisons().size());
        for (const auto& comparison : candidate.graph().bindingComparisons()) selected.push_back(same_binding(comparison));
        candidate.projection_subscriptions.reserve(manifest.gpu_imports.size());
        for (std::size_t i = 0; i < slots.size(); ++i)
        {
            const auto gpu = slots[i].gpu_import;
            if (gpu == kGraphInvalidIndex) continue;
            const auto& compiled = manifest.gpu_imports[gpu].slot;
            const auto slot = compiled.resource.index;
            if (slot == kGraphInvalidIndex) continue;
            const auto& input = resources[i];
            if (!input.factory) return cxx::unexpected(RenderError{kStorageIdentity, {i}});
            ProjectionSubscription link{input.id, slots[i].projection};
            if (!input.publication) candidate.useFrameResource(input.frameSource());
            const auto& recording = candidate.graph().recordingProgram();
            const auto selected_region = [&](std::uint32_t region) noexcept
            {
                const auto& guard = recording.conditions[region];
                return bool(selected[guard.input_slot]) == guard.expected;
            };
            const bool may_access = !compiled.output_range || compiled.unconditional_access ||
                std::ranges::any_of(compiled.binding_regions, selected_region);
            const ResourceRegistry::GraphBindingUse user{
                candidate.binding_identity, &candidate.frame_bindings, &compiled, may_access
            };
            if (auto bound = runtime_->resources_.stageProjection(installation, link, user, input.publication); !bound)
                return bound;
            if (!input.publication) candidate.projection_subscriptions.push_back(link);
            if (compiled.output_range || input.publication)
            {
                auto& product = candidate.buffer_products.emplace_back(input.id, link.projection, &compiled);
                if (input.publication) product.publication = *input.publication;
            }
        }
        if (auto ready = candidate.bindPredicates(std::move(selected)); !ready) return ready;
        if (auto ready = candidate.initialize(); !ready) return ready;
        const auto& preparation = candidate.device_graph->algorithms;
        for (const auto& uniform : candidate.device_graph->constant_buffers)
            if (!*uniform.value.revision) return cxx::unexpected(RenderError{kFrameContract, {uniform.resource.index}});
        candidate.view_sources.reserve(preparation.resource_view_slots.size());
        for (const auto slot : preparation.resource_view_slots)
        {
            const auto& source = resources[slot];
            if (!source.factory) return cxx::unexpected(RenderError{kStorageIdentity, {slot}});
            candidate.view_sources.push_back(candidate.useFrameResource(source.frameSource()));
        }
        candidate.draw_resource_ids.reserve(preparation.draw_resource_bindings.size());
        for (const auto& input : preparation.draw_resource_bindings)
        {
            const auto& operand = resources[input.binding];
            candidate.draw_resource_ids.push_back(operand.id);
            if (operand.factory) candidate.useFrameResource(operand.frameSource());
        }
        if (auto ready = candidate.bindFrameAlgorithms(resources); !ready) return ready;
        if (auto values = candidate.setConditions(input.conditions); !values) return values;
        candidate.viewport = input.viewport;
        return {};
    }
} // namespace lux::render
