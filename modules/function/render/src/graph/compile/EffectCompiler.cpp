#include <lux/engine/render/device/SubmissionQueue.hpp>
#include <lux/engine/render/graph/execute/RasterStreamPlan.hpp>
#include <lux/engine/render/device/NativeResult.hpp>
#include <lux/engine/render/graph/RenderGraph.hpp>
#include <lux/engine/render/graph/Range.hpp>
#include <lux/engine/render/graph/compile/EffectCompileRequest.hpp>
#include <lux/engine/render/graph/compile/GraphCompiler.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/runtime/commands/Error.hpp>

namespace lux::render
{
    RasterOperands detail::CompiledRasterStream::operands(
        GraphPassId pass, DrawStreamSlot stream, const RasterPassTemplate& layout, std::uint32_t first_source
    ) const & noexcept
    {
        return {{pass, stream}, layout, *this, first_source};
    }

    RenderResult<extension_detail::DrawSourceAddress>
    RasterOperands::source(std::string_view name, cxx::basic_type_info type) const noexcept
    {
        const auto& inputs = layout_.draw_resources;
        const auto found = std::ranges::find(inputs, name, &DrawResourceInput::binding);
        if (found == inputs.end()) return cxx::unexpected(RenderError{kGraphUnknownResource, {pass_.source.index}});
        const auto declared = found->source.implementation;
        if (declared != type || declared.name() != type.name())
            return cxx::unexpected(RenderError{kStorageMismatch, {pass_.source.index}});
        return extension_detail::DrawSourceAddress{
            pass_.compiled.index, first_source_ + static_cast<std::uint32_t>(found - inputs.begin())
        };
    }

    RenderResult<RasterProgramSlot> RasterOperands::program(std::uint32_t id) const noexcept
    {
        const auto programs = layout_.fixedPrograms();
        const auto use = std::ranges::find(programs, id, &RasterPassTemplate::StreamProgram::program);
        if (use == programs.end())
            return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {pass_.source.index, id}});
        return RasterProgramSlot{pass_.compiled.index, static_cast<std::uint32_t>(use - programs.begin())};
    }

    RenderResult<RasterBufferSlot> RasterOperands::buffer(GraphResourceId id) const noexcept
    {
        using Access = detail::CompiledRasterStream::BufferAccess;
        const auto& rows = access_.buffers;
        const auto first = std::ranges::lower_bound(rows, id.index, {}, &Access::resource);
        if (first == rows.end() || first->resource != id.index)
            return cxx::unexpected(RenderError{kGraphUnknownResource, {pass_.source.index, id.index}});
        return RasterBufferSlot{static_cast<std::uint32_t>(first - rows.begin()), pass_.compiled.index};
    }

    RenderResult<RasterImageSlot> RasterOperands::image(GraphResourceId id) const noexcept
    {
        using Access = detail::CompiledRasterStream::ImageAccess;
        const auto& rows = access_.images;
        const auto first = std::ranges::lower_bound(rows, id.index, {}, &Access::resource);
        if (first == rows.end() || first->resource != id.index)
            return cxx::unexpected(RenderError{kGraphUnknownResource, {pass_.source.index, id.index}});
        return RasterImageSlot{static_cast<std::uint32_t>(first - rows.begin()), pass_.compiled.index};
    }

    RenderResult<RasterSamplerSlot> RasterOperands::sampler(std::uint32_t id) const noexcept
    {
        const auto& samplers = layout_.stream_samplers;
        const auto use = std::ranges::find(samplers, id);
        if (use == samplers.end())
            return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {pass_.source.index, id}});
        return RasterSamplerSlot{pass_.compiled.index, static_cast<std::uint32_t>(use - samplers.begin())};
    }

    RenderResult<std::uint32_t> RasterOperands::argument(std::string_view name) const noexcept
    {
        const auto& inputs = layout_.inputs;
        const auto use = std::ranges::find(inputs, name, &RasterPassTemplate::Input::name);
        if (use == inputs.end() || !use->kind)
            return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {pass_.source.index}});
        return static_cast<std::uint32_t>(use - inputs.begin());
    }
}

namespace lux::render::detail
{
    using namespace runtime_detail;

    namespace
    {
        struct PreparationLinkWorkspace final
        {
            EffectGraphBuild& build;
            const detail::PreparedRenderGraph& compiled;
            std::vector<std::uint32_t> data_slots, snapshot_slots;
            std::span<const EffectInputDefinition> effect_inputs;
            std::vector<CompiledPassSlot> pass_slots;
            CpuProgram& plan;
            std::vector<RasterOperands> streams;

            PreparationLinkWorkspace(
                EffectGraphBuild& graph, const detail::PreparedRenderGraph& native,
                std::span<const EffectInputDefinition> inputs, CpuProgram& program
            ) noexcept
                : build(graph), compiled(native), data_slots(build.cpu_data.size(), kGraphInvalidIndex),
                  snapshot_slots(build.snapshots.size(), kGraphInvalidIndex),
                  effect_inputs(inputs),
                  pass_slots(graph.source.passes().size()),
                  plan(program)
            {
                const auto& order = graph.analysis.execution_order;
                for (std::uint32_t slot = 0; slot < order.size(); ++slot) pass_slots[order[slot].index] = {slot};
                for (auto producer : build.live_order)
                    for (auto input : build.producers[producer].declaration.storage_inputs) snapshotSlot(input);
                plan.frame_snapshot_count = static_cast<std::uint32_t>(plan.snapshots.size());
            }

            std::array<CompiledPassSlot, 2> destinations(GraphPassId id) const noexcept
            {
                std::array<CompiledPassSlot, 2> slots{pass_slots[id.index], CompiledPassSlot{}};
                const auto peer = build.source.passes()[id.index].choice_peer;
                if (peer.index != kGraphInvalidIndex) slots[1] = pass_slots[peer.index];
                if (slots[0].index == kGraphInvalidIndex) std::swap(slots[0], slots[1]);
                return slots;
            }

            std::uint32_t snapshotSlot(GraphStorageSnapshotId input) noexcept
            {
                auto& slot = snapshot_slots[input.index];
                if (slot == kGraphInvalidIndex)
                {
                    const auto& snapshot = build.snapshots[input.index];
                    slot = static_cast<std::uint32_t>(plan.snapshots.size());
                    plan.snapshots.push_back({snapshot.type, snapshot.binding});
                }
                return slot;
            }

            RenderResult<void>
            copyInline(
                GraphPassId pass, InputSlot input, std::uint32_t bytes, std::uint32_t revision = kGraphInvalidIndex
            ) noexcept
            {
                for (auto destination : destinations(pass))
                {
                    if (destination.index == kGraphInvalidIndex) continue;
                    if (!bytes || compiled.graph.passes()[destination.index].parameter_size != bytes)
                        return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {pass.index}});
                    plan.constant_copies.push_back({input, destination, bytes, revision});
                }
                return {};
            }

            RenderResult<InputSlot>
            effectValue(const EffectInputDefinition& read, decltype(CpuProgram::EffectInput::bind) member = nullptr) noexcept
            {
                const auto state = std::ranges::find(effect_inputs, read.node, &EffectInputDefinition::node);
                const auto invalid = RenderError{kGraphInvalidDescriptor, {read.node.value}};
                if (state == effect_inputs.end()) return cxx::unexpected(invalid);
                const bool matches_type = state->type == read.type && state->type.name() == read.type.name();
                if (!matches_type || (!member && state->bytes != read.bytes))
                    return cxx::unexpected(invalid);

                const auto source = static_cast<std::uint32_t>(state - effect_inputs.begin());
                const auto existing = std::ranges::find_if(plan.effect_inputs, [&](const auto& input) noexcept
                {
                    return input.source == source && input.bind == member;
                });
                if (existing != plan.effect_inputs.end()) return existing->destination;
                if (plan.cpu_values.size() == kGraphInvalidIndex) return cxx::unexpected(RenderError{kRuntimeCapacity});
                const InputSlot slot{static_cast<std::uint32_t>(plan.cpu_values.size())};
                plan.cpu_values.push_back({read.bytes});
                plan.effect_inputs.push_back({source, slot, member});
                return slot;
            }

        };

        class CpuCallLink final
        {
        public:
            CpuCallLink(PreparationLinkWorkspace& workspace, const CpuCallDependencies& producer) noexcept
                : workspace_(workspace), producer_(producer) {}

            RenderResult<void> link(std::uint32_t source_index, rdesc::EShaderTarget target) noexcept
            {
                auto& plan = workspace_.plan;
                const auto input_count = producer_.inputs.size();
                if (input_count > kGraphInvalidIndex - plan.cpu_reads.size())
                    return cxx::unexpected(RenderError{kRuntimeCapacity});
                const auto first_input = static_cast<std::uint32_t>(plan.cpu_reads.size());
                if (producer_.storage_inputs.size() > kGraphInvalidIndex - plan.snapshot_reads.size())
                    return cxx::unexpected(RenderError{kRuntimeCapacity});
                const auto first_snapshot = static_cast<std::uint32_t>(plan.snapshot_reads.size());
                const auto first_stream = static_cast<std::uint32_t>(plan.streams.size());
                auto& source = workspace_.build.producers[source_index];
                workspace_.streams.clear();
                for (auto id : producer_.draw_streams)
                    if (auto linked = linkStream(id); !linked) return linked;
                auto call = source.bind(std::move(source.configuration), workspace_.streams);
                if (!call) return cxx::unexpected(call.error());
                if (auto inputs = linkInputs(call->methods->inputs, source_index); !inputs) return inputs;
                const auto first_output = static_cast<std::uint32_t>(plan.cpu_values.size());
                if (auto outputs = linkResults(call->methods->outputs, source_index, target); !outputs) return outputs;

                const auto stream_count = static_cast<std::uint32_t>(plan.streams.size()) - first_stream;
                const auto first_buffer = static_cast<std::uint32_t>(plan.buffers.size());
                for (const auto outputs : {std::span{producer_.uploads}, std::span{producer_.frame_sizes}})
                {
                    for (auto id : outputs)
                    {
                        const auto& buffer = *workspace_.build.findBuffer(id);
                        const bool used = workspace_.compiled.graph.resources()[id.index].memory_used;
                        plan.buffers.push_back({
                            id.index, buffer.minimum_bytes, buffer.maximum_bytes, buffer.upload, used
                        });
                    }
                }
                CpuCall linked{
                    .call = std::move(*call),
                    .streams = {first_stream, stream_count},
                    .buffers = {first_buffer, static_cast<std::uint32_t>(plan.buffers.size()) - first_buffer},
                    .upload_count = static_cast<std::uint32_t>(producer_.uploads.size()),
                    .first_snapshot = first_snapshot,
                    .first_input = first_input,
                    .first_output = first_output
                };
                if (program_source_)
                {
                    if (plan.raster_builds.size() == kGraphInvalidIndex) return cxx::unexpected(RenderError{kRuntimeCapacity});
                    linked.program_slot = static_cast<std::uint32_t>(plan.raster_builds.size());
                    auto& recipe = plan.raster_builds.emplace_back(static_cast<std::uint32_t>(plan.calls.size()));
                    if (!recipe.link(program_source_->builder, linked.call))
                        return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {source_index}});
                    if (program_source_->snapshots.size() > kGraphInvalidIndex - plan.program_reads.size())
                        return cxx::unexpected(RenderError{kRuntimeCapacity});
                    recipe.snapshots = {
                        static_cast<std::uint32_t>(plan.program_reads.size()),
                        static_cast<std::uint32_t>(program_source_->snapshots.size())
                    };
                    for (auto id : program_source_->snapshots) plan.program_reads.push_back(workspace_.snapshotSlot(id));
                }
                if (linked.call.methods->create_workspace)
                {
                    if (plan.workspace_count == kGraphInvalidIndex)
                        return cxx::unexpected(RenderError{kRuntimeCapacity});
                    linked.workspace_slot = plan.workspace_count++;
                }
                plan.calls.push_back(std::move(linked));
                return {};
            }

        private:
            RenderResult<void> linkStream(GraphPassId pass) noexcept
            {
                const auto slot = workspace_.pass_slots[pass.index].index;
                const auto& graph = workspace_.compiled.graph;
                const auto* stream = slot < graph.passes().size() ? graph.programs().rasterStream(slot) : nullptr;
                if (!stream) return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {pass.index}});
                auto& plan = workspace_.plan;
                if (plan.draw_resource_ranges.empty()) plan.draw_resource_ranges.resize(graph.passes().size());
                auto& range = plan.draw_resource_ranges[slot];
                if (range.producer != kGraphInvalidIndex)
                    return cxx::unexpected(RenderError{kGraphAmbiguousProducer, {pass.index}});
                const auto& work = std::get<RasterWork>(workspace_.build.source.passes()[pass.index].command);
                const auto& layout = std::get<RasterPassTemplate>(work.program);
                if (const auto* source = std::get_if<RasterProgramSource>(&layout.programs))
                {
                    if (program_source_ && *program_source_ != *source)
                        return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {pass.index}});
                    program_source_ = source;
                }
                const auto first = plan.draw_resource_bindings.size();
                if (layout.draw_resources.size() > kGraphInvalidIndex - first)
                    return cxx::unexpected(RenderError{kRuntimeCapacity});
                range.first = static_cast<std::uint32_t>(first);
                range.count = static_cast<std::uint32_t>(layout.draw_resources.size());
                range.producer = static_cast<std::uint32_t>(plan.calls.size());
                range.list = stream->list;
                for (const auto& input : layout.draw_resources)
                {
                    const auto& imports = workspace_.build.storage_imports;
                    const auto it = std::ranges::lower_bound(imports, input.binding, {}, &EffectStorageImport::binding);
                    if (it == imports.end() || it->binding != input.binding)
                        return cxx::unexpected(RenderError{kGraphUnknownResource, {pass.index}});
                    plan.draw_resource_bindings.push_back({
                        static_cast<std::uint32_t>(it - imports.begin()),
                        input.required_capabilities, input.indexed_geometry
                    });
                }
                plan.streams.push_back({slot, stream->list});
                workspace_.streams.push_back(stream->operands(pass, {slot}, layout, range.first));
                return {};
            }

            bool matches(const extension_detail::CpuValueType& port, GraphCpuDataId id) const noexcept
            {
                const auto& declared = workspace_.build.cpu_data[id.index];
                return declared.bytes == port.bytes && declared.type == port.type &&
                    declared.type.name() == port.type.name();
            }

            RenderResult<void>
            linkInputs(std::span<const extension_detail::CpuValueType> inputs, std::uint32_t source) noexcept
            {
                const auto invalid = RenderError{kGraphInvalidDescriptor, {source}};
                std::size_t value = 0, snapshot = 0;
                for (const auto& port : inputs)
                {
                    if (port.bytes)
                    {
                        if (value == producer_.inputs.size()) return cxx::unexpected(invalid);
                        const auto id = producer_.inputs[value++];
                        if (!matches(port, id))
                            return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {source, id.index}});
                        auto& slot = workspace_.data_slots[id.index];
                        if (slot == kGraphInvalidIndex)
                        {
                            auto input = workspace_.effectValue(workspace_.build.cpu_data[id.index]);
                            if (!input) return cxx::unexpected(input.error());
                            slot = input->index;
                        }
                        workspace_.plan.cpu_reads.push_back({slot});
                    }
                    else
                    {
                        if (snapshot == producer_.storage_inputs.size()) return cxx::unexpected(invalid);
                        const auto id = producer_.storage_inputs[snapshot++].index;
                        const auto declared = workspace_.build.snapshots[id].type;
                        if (port.type != declared || port.type.name() != declared.name())
                            return cxx::unexpected(RenderError{kStorageMismatch, {source, id}});
                        workspace_.plan.snapshot_reads.push_back(workspace_.snapshot_slots[id]);
                    }
                }
                if (value != producer_.inputs.size() || snapshot != producer_.storage_inputs.size())
                    return cxx::unexpected(invalid);
                return {};
            }

            RenderResult<void>
            linkResults(
                std::span<const extension_detail::CpuResultType> results,
                std::uint32_t source, rdesc::EShaderTarget target
            ) noexcept
            {
                const auto values = producer_.outputs.size();
                if (values > results.size() || results.size() - values != producer_.inline_blocks.size())
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {source}});
                auto& plan = workspace_.plan;
                if (results.size() > kGraphInvalidIndex - plan.cpu_values.size())
                    return cxx::unexpected(RenderError{kRuntimeCapacity});
                for (std::size_t i = 0; i < results.size(); ++i)
                {
                    const auto& result = results[i];
                    const InputSlot slot{static_cast<std::uint32_t>(plan.cpu_values.size())};
                    plan.cpu_values.push_back({result.value.bytes});
                    if (i < values)
                    {
                        const auto id = producer_.outputs[i];
                        if (!matches(result.value, id))
                            return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {source, id.index}});
                        workspace_.data_slots[id.index] = slot.index;
                        continue;
                    }
                    const auto pass = producer_.inline_blocks[i - values];
                    const auto* parameters = workspace_.build.source.passes()[pass.index].bindings().parameters;
                    const bool compatible = result.block && parameters &&
                        sameBlockContract(parameters->inline_constant, *result.block, target);
                    if (!compatible) return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {pass.index}});
                    auto copy = workspace_.copyInline(pass, slot, result.block->bytes, result.revision_offset);
                    if (!copy) return copy;
                }
                return {};
            }

            PreparationLinkWorkspace& workspace_;
            const CpuCallDependencies& producer_;
            const RasterProgramSource* program_source_ = nullptr;
        };

        RenderResult<void> linkPreparation(PreparationLinkWorkspace workspace, rdesc::EShaderTarget target) noexcept
        {
            const auto& build = workspace.build;
            const auto& executable = workspace.compiled.graph;
            auto& plan = workspace.plan;
            for (const auto& copy : build.inline_copies)
            {
                const auto slots = workspace.destinations(copy.pass);
                if (slots.front().index == kGraphInvalidIndex) continue;
                auto input = workspace.effectValue(
                    {copy.node, copy.input.state_type, copy.input.block.bytes}, copy.input.bind
                );
                if (!input) return cxx::unexpected(input.error());
                if (auto added = workspace.copyInline(copy.pass, *input, copy.input.block.bytes); !added) return added;
            }
            plan.calls.reserve(build.live_order.size());
            for (auto index : build.live_order)
            {
                const auto& producer = build.producers[index];
                CpuCallLink context{workspace, producer.declaration};
                auto linked = context.link(index, target);
                if (!linked) return cxx::unexpected(linked.error());
            }
            for (std::uint32_t p = 0; p < executable.passes().size(); ++p)
            {
                const auto& grants = plan.draw_resource_ranges;
                if (!executable.programs().rasterStream(p)) continue;
                if (grants.empty() || grants[p].producer == kGraphInvalidIndex)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {p}});
            }
            const auto place = [&](CpuByteRange& range)
            {
                if (range.size > std::vector<std::byte>{}.max_size() - plan.byte_count) return false;
                range.offset = plan.byte_count;
                plan.byte_count += range.size;
                return true;
            };
            for (auto& value : plan.cpu_values)
                if (!place(value)) return cxx::unexpected(RenderError{kRuntimeCapacity});
            return {};
        }

        struct LinkedBufferOutputs final
        {
            std::vector<FinalBufferUse> uses;
            std::vector<FinalBufferExport> exports;
        };

        RenderResult<LinkedBufferOutputs> linkBufferOutputs(
            EffectGraphBuild& build, const compiled::RenderGraph& executable, DeviceSession& session
        ) noexcept
        {
            auto& slots = build.storage_imports;
            LinkedBufferOutputs result;
            if (slots.empty()) return result;
            std::vector<std::uint32_t> owner(build.source.resources().size(), kGraphInvalidIndex);
            for (std::uint32_t i = 0; i < slots.size(); ++i)
            {
                const auto resource = slots[i].slot.resource.index;
                if (resource == kGraphInvalidIndex) continue;
                if (std::holds_alternative<GraphBufferDesc>(build.source.resources()[resource].description))
                {
                    owner[resource] = i;
                    slots[i].slot.boundary.use = build.source.resources()[resource].initial_use;
                }
            }
            std::vector<std::vector<FinalBufferUse>> uses(slots.size());
            std::vector<bool> preserves_boundary(slots.size(), true);
            const auto observe = [&](GraphResourceId id, std::uint32_t pass, const ResourceAccess& use, bool write)
            {
                const auto binding = owner[id.index];
                if (binding == kGraphInvalidIndex) return;
                if (slots[binding].slot.output_range)
                    uses[binding].push_back({pass, std::get<GraphBufferRange>(use.range), write});
                if (!preserves_boundary[binding]) return;
                auto& boundary = slots[binding].slot.boundary;
                const auto physical = session.queueIndex(session.queue(executable.passes()[pass].queue));
                const bool same_queue = boundary.queue == kGraphInvalidIndex || boundary.queue == physical;
                preserves_boundary[binding] = same_queue && use.usage == boundary.use;
                boundary.queue = preserves_boundary[binding] ? static_cast<std::uint32_t>(physical) : kGraphInvalidIndex;
            };
            const auto& order = build.analysis.execution_order;
            for (std::uint32_t p = 0; p < order.size(); ++p)
            {
                for (const auto& use : build.source.passes()[order[p].index].uses)
                {
                    observe(use.version.resource, p, use, writes(use.access));
                    if (use.fallback) observe(use.fallback->resource, p, use, false);
                }
            }
            std::vector<std::vector<FinalBufferExport>> exports(slots.size());
            for (const auto& output : build.source.outputs())
            {
                std::optional<PassCondition> guard;
                if (output.alternate)
                {
                    guard = output.alternate->primary_when;
                    if (guard->source == EConditionSource::HISTORY_VALID)
                    {
                        guard->input = executable.resources()[guard->input].history;
                        if (guard->input == kGraphInvalidIndex)
                            return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                    }
                }
                const auto append = [&](ResourceVersion version)
                {
                    const auto binding = owner[version.resource.index];
                    if (binding != kGraphInvalidIndex && slots[binding].slot.output_range)
                        exports[binding].push_back({std::get<GraphBufferRange>(output.range), guard});
                };
                append(output.version);
                if (output.alternate)
                {
                    guard->expected = !guard->expected;
                    append(output.alternate->value);
                }
            }
            for (std::size_t i = 0; i < slots.size(); ++i)
            {
                auto& binding = slots[i].slot;
                if (binding.boundary.queue != kGraphInvalidIndex)
                {
                    const auto& resource = executable.resources()[binding.resource.index];
                    const auto queue_of = [&](EQueueRole role) { return session.queueIndex(session.queue(role)); };
                    const bool foreign_restore = !resource.final_barrier_uses.empty() &&
                        queue_of(executable.submissions().back().queue) != binding.boundary.queue;
                    const bool foreign_barrier = std::ranges::any_of(resource.barrier_uses, [&](const auto& use)
                    {
                        return queue_of(executable.passes()[use.first].queue) != binding.boundary.queue;
                    });
                    if (foreign_restore || foreign_barrier) binding.boundary.queue = kGraphInvalidIndex;
                }
                binding.first_use = static_cast<std::uint32_t>(result.uses.size());
                binding.use_count = static_cast<std::uint32_t>(uses[i].size());
                result.uses.insert(result.uses.end(), uses[i].begin(), uses[i].end());
                binding.first_export = static_cast<std::uint32_t>(result.exports.size());
                binding.export_count = static_cast<std::uint32_t>(exports[i].size());
                result.exports.insert(result.exports.end(), exports[i].begin(), exports[i].end());
            }
            return result;
        }
    } // namespace

    exec::task<GraphCompiler::EffectResult>
    GraphCompiler::compileEffect(
        DeviceSession& session, EffectCompileRequest request, EffectCompileEnvironment env
    ) noexcept
    {
        // Evaluated on the control owner after child work rejoins.
        const auto interrupted = [&]() noexcept -> RenderError
        {
            if (env.stopping.load()) return {kCommandCancelled};
            return env.device_failure.type ? env.device_failure : session.terminalError();
        };
        const auto& features = session.enabledFeatures();
        const auto& limits = session.description();
        const auto& dispatch = limits.shaderStage.compute.dispatchMaxDim;
        const EffectBuildDevice device{
            features.shader_target, session.usesDeviceAddresses(), features.indirect_first_instance,
            {dispatch[0], dispatch[1], dispatch[2]}, limits.memory.storageBufferMaxRange,
            limits.dimensions.textureLayerMaxNum, features.texture_descriptors,
            (session.resourceCapabilities() & static_cast<std::uint32_t>(EStorageCapability::STORAGE_BUFFER_ARRAY)) != 0
        };
        const auto owner = co_await stdexec::read_env(stdexec::get_scheduler);
        auto built = co_await (
            stdexec::schedule(env.cpu) |
            stdexec::then([&]() noexcept
            {
                return lowerEffectGraph(
                    request.recipe, env.catalog, {request.output, device, request.target_final_use}, env.diagnostics
                );
            }) |
            stdexec::upon_stopped([]() noexcept -> RenderResult<EffectGraphBuild>
            {
                return cxx::unexpected(RenderError{kCpuStopped});
            }) |
            stdexec::continues_on(owner) |
            stdexec::upon_error(detail::NativeDeliveryFailure<EffectGraphBuild>{})
        );
        if (!built) co_return cxx::unexpected(built.error());
        if (const auto failure = interrupted(); failure.type) co_return cxx::unexpected(failure);

        std::vector<GraphResourceId> concurrent_reads;
        for (std::size_t i = 0; i < built->storage_imports.size(); ++i)
        {
            const auto resource = built->storage_imports[i].slot.resource;
            if (resource.index == kGraphInvalidIndex) continue;
            const auto* storage = std::get_if<StorageGraphImport>(&built->storage_imports[i].contract);
            if (storage && storage->concurrent_image_read) concurrent_reads.push_back(resource);
        }
        auto plan = co_await compile(
            session, env.cpu, {built->source, built->analysis, concurrent_reads}, {.target = request.output}
        );
        if (!plan) co_return cxx::unexpected(plan.error());
        if (const auto failure = interrupted(); failure.type) co_return cxx::unexpected(failure);

        std::vector<compiled::BindingComparison> comparisons;
        const auto resolve_operand = [&](GraphResourceId id) noexcept -> RenderResult<compiled::BindingOperand>
        {
            using Domain = compiled::BindingOperand::EDomain;
            const auto targets = std::span{built->target_imports};
            const auto target = std::ranges::find(targets, id, &TargetImageSlot::resource);
            if (target != targets.end())
                return compiled::BindingOperand{Domain::TARGET, static_cast<std::uint32_t>(target - targets.begin())};
            const auto storages = std::span{built->storage_imports};
            const auto storage = std::ranges::find(storages, id, [](const auto& input) { return input.slot.resource; });
            if (storage != storages.end())
                return compiled::BindingOperand{Domain::STORAGE, static_cast<std::uint32_t>(storage - storages.begin())};
            return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {id.index}});
        };
        comparisons.reserve(built->source.bindingPredicates().size());
        for (const auto& predicate : built->source.bindingPredicates())
        {
            auto left = resolve_operand(predicate.left);
            if (!left) co_return cxx::unexpected(left.error());
            auto right = resolve_operand(predicate.right);
            if (!right) co_return cxx::unexpected(right.error());
            comparisons.push_back({*left, *right});
        }
        for (auto& input : built->target_imports)
        {
            auto& slot = input.resource;
            const auto& resource = plan->graph.resources()[slot.index];
            if (!resource.memory_used) slot = {};
            else if (!resource.sampled_target_input)
                co_return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {slot.index}});
            if (slot.index == kGraphInvalidIndex) continue;
            const auto batches = plan->graph.submissions();
            for (std::uint32_t b = 0; b < batches.size(); ++b)
            {
                const auto& batch = batches[b];
                if (!std::ranges::binary_search(batch.imported_resources, slot.index, {}, &GraphResourceId::index))
                    continue;
                auto& first = input.first_reads.batches[session.queueIndex(session.queue(batch.queue))];
                first = std::min(first, b);
            }
            if (std::ranges::all_of(input.first_reads.batches, [](auto b) { return b == kGraphInvalidIndex; }))
                co_return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {slot.index}});
        }
        for (auto& declaration : built->storage_imports)
        {
            auto& slot = declaration.slot;
            const auto id = slot.resource.index;
            if (id != kGraphInvalidIndex && !plan->graph.resources()[id].memory_used) slot = {};
        }
        auto preparation = linkPreparation(
            {*built, *plan, request.inputs, plan->graph.cpu_program_}, features.shader_target
        );
        if (!preparation) co_return cxx::unexpected(preparation.error());
        for (const auto& block : built->uniforms)
        {
            if (!plan->graph.resources()[block.resource.index].memory_used) continue;
            if (!request.uploads_available) co_return cxx::unexpected(RenderError{kDeviceUnsupported});
            const auto state = std::ranges::find(request.inputs, block.node, &EffectInputDefinition::node);
            if (state == request.inputs.end() || state->type != block.source.state_type)
                co_return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {block.node.value, block.resource.index}});
            const auto source = static_cast<std::uint32_t>(state - request.inputs.begin());
            plan->graph.cpu_program_.constant_buffers.push_back({source, block.source.block.bytes, block.resource, block.source.bind});
        }
        if (const auto failure = interrupted(); failure.type) co_return cxx::unexpected(failure);
        auto outputs = linkBufferOutputs(*built, plan->graph, session);
        if (!outputs) co_return cxx::unexpected(outputs.error());
        auto& graph = plan->graph;
        graph.target_imports_ = std::move(built->target_imports);
        graph.storage_imports_ = std::move(built->storage_imports);
        graph.output_uses_ = std::move(outputs->uses);
        graph.buffer_exports_ = std::move(outputs->exports);
        graph.binding_comparisons_ = std::move(comparisons);
        co_return std::make_unique<const compiled::RenderGraph>(std::move(graph));
    }
} // namespace lux::render::detail
