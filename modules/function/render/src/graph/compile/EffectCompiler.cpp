#include <lux/engine/render/graph/execute/RasterStreamPlan.hpp>
#include <lux/engine/render/device/NativeResult.hpp>
#include <lux/engine/render/graph/RenderGraph.hpp>
#include <lux/engine/render/graph/Range.hpp>
#include <lux/engine/render/graph/compile/EffectCompileRequest.hpp>
#include <lux/engine/render/graph/compile/GraphCompiler.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/commands/Error.hpp>

namespace lux::render::detail
{
    using namespace runtime_detail;

    namespace
    {
        struct PreparationLinkWorkspace final
        {
            const EffectGraphBuild& build;
            const detail::PreparedRenderGraph& compiled;
            std::vector<std::uint32_t> data_slots, buffer_slots, snapshot_slots;
            std::span<const EffectInputDefinition> effect_inputs;
            std::vector<std::uint32_t> effect_slots;
            std::vector<CompiledPassSlot> pass_slots;
            CpuProgram& plan;

            PreparationLinkWorkspace(
                const EffectGraphBuild& graph, const detail::PreparedRenderGraph& native,
                std::span<const EffectInputDefinition> inputs, CpuProgram& program
            ) noexcept
                : build(graph), compiled(native), data_slots(build.cpu_data.size(), kGraphInvalidIndex),
                  buffer_slots(build.source.resources().size(), kGraphInvalidIndex),
                  snapshot_slots(build.snapshots.size(), kGraphInvalidIndex),
                  effect_inputs(inputs), effect_slots(inputs.size(), kGraphInvalidIndex),
                  pass_slots(graph.source.passes().size()),
                  plan(program)
            {
                const auto& order = graph.analysis.execution_order;
                for (std::uint32_t slot = 0; slot < order.size(); ++slot) pass_slots[order[slot].index] = {slot};
                assignSlots();
            }

            std::array<CompiledPassSlot, 2> destinations(GraphPassId id) const noexcept
            {
                std::array<CompiledPassSlot, 2> slots{pass_slots[id.index], CompiledPassSlot{}};
                const auto peer = build.source.passes()[id.index].choice_peer;
                if (peer.index != kGraphInvalidIndex) slots[1] = pass_slots[peer.index];
                if (slots[0].index == kGraphInvalidIndex) std::swap(slots[0], slots[1]);
                return slots;
            }

        private:
            void assignSlots() noexcept
            {
                const auto& definition = build;
                for (auto producer : definition.live_order)
                {
                    for (auto input : definition.producers[producer].declaration.storage_inputs)
                    {
                        auto& slot = snapshot_slots[input.index];
                        if (slot != kGraphInvalidIndex) continue;
                        const auto& snapshot = definition.snapshots[input.index];
                        slot = static_cast<std::uint32_t>(plan.snapshots.size());
                        plan.snapshots.push_back({snapshot.type, snapshot.binding});
                    }
                    for (auto output : definition.producers[producer].declaration.outputs)
                    {
                        data_slots[output.index] = static_cast<std::uint32_t>(plan.cpu_values.size());
                        plan.cpu_values.push_back({definition.cpu_data[output.index].bytes});
                    }
                    for (auto output : definition.producers[producer].declaration.gpu_buffers)
                    {
                        buffer_slots[output.index] = static_cast<std::uint32_t>(plan.buffers.size());
                        plan.buffers.emplace_back();
                    }
                }
                for (const auto& buffer : definition.buffers)
                {
                    const auto slot = buffer_slots[buffer.resource.index];
                    if (slot != kGraphInvalidIndex)
                        plan.buffers[slot] = {buffer.resource.index, buffer.minimum_bytes, buffer.maximum_bytes,
                            buffer.upload, compiled.graph.resources()[buffer.resource.index].memory_used};
                }
            }
        };

        class LinkContext final : public InstanceLinkContext
        {
        public:
            LinkContext(
                PreparationLinkWorkspace& workspace,
                const GraphPreparation& producer,
                std::uint32_t source,
                std::uint32_t slot
            ) noexcept : workspace_(workspace), producer_(producer), source_input_(source), producer_slot_(slot)
            {
            }

            RenderResult<void> link(std::uint32_t source_index) noexcept
            {
                auto& plan = workspace_.plan;
                const auto input_count = producer_.inputs.size();
                if (input_count > kGraphInvalidIndex - plan.cpu_reads.size())
                    return cxx::unexpected(RenderError{kRuntimeCapacity});
                inputs_.first = static_cast<std::uint32_t>(plan.cpu_reads.size());
                for (auto id : producer_.inputs) plan.cpu_reads.push_back({workspace_.data_slots[id.index]});
                inputs_.count = static_cast<std::uint32_t>(input_count);
                constants_.first = static_cast<std::uint32_t>(plan.inline_blocks.size());
                if (producer_.storage_inputs.size() > kGraphInvalidIndex - plan.snapshot_reads.size())
                    return cxx::unexpected(RenderError{kRuntimeCapacity});
                snapshots_.first = static_cast<std::uint32_t>(plan.snapshot_reads.size());
                snapshots_.count = static_cast<std::uint32_t>(producer_.storage_inputs.size());
                for (auto id : producer_.storage_inputs)
                    plan.snapshot_reads.push_back({workspace_.snapshot_slots[id.index], kGraphInvalidIndex, nullptr});
                const auto first_program_read = static_cast<std::uint32_t>(plan.program_reads.size());
                for (auto id : producer_.program_storage_inputs)
                {
                    const auto found = std::ranges::find(producer_.storage_inputs, id);
                    if (found == producer_.storage_inputs.end())
                        return cxx::unexpected(RenderError{kGraphUnknownResource, {source_index, id.index}});
                    const auto local = static_cast<std::uint32_t>(found - producer_.storage_inputs.begin());
                    const auto operand = snapshots_.first + local;
                    plan.snapshot_reads[operand].program = static_cast<std::uint32_t>(plan.program_reads.size()) - first_program_read;
                    plan.program_reads.push_back(operand);
                }
                const auto first_stream = static_cast<std::uint32_t>(plan.streams.size());
                const auto buffer_count = static_cast<std::uint32_t>(producer_.gpu_buffers.size());
                const auto first_buffer = buffer_count ? workspace_.buffer_slots[producer_.gpu_buffers.front().index] : 0u;
                auto call = producer_.algorithm.link(producer_.configuration, *this);
                if (!call) return cxx::unexpected(call.error());
                const auto expected = std::ranges::count_if(producer_.inline_blocks, [&](GraphPassId pass) noexcept
                {
                    return workspace_.destinations(pass)[0].index != kGraphInvalidIndex;
                });
                const bool missing_constants = constants_.count != static_cast<std::size_t>(expected);
                const bool missing_buffers = claimed_buffers_ != buffer_count;
                if (missing_constants || missing_buffers)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {source_index}});

                const auto& builder = producer_.program_builder;
                const auto streams = std::span{plan.streams}.subspan(first_stream);
                const bool dynamic = std::ranges::any_of(streams, [&](const auto& stream) noexcept
                {
                    return workspace_.compiled.graph.programs().rasterStream(stream.pass)->runtime.has_value();
                });
                const bool invalid_builder = dynamic != bool(builder);
                if (invalid_builder || (!dynamic && !producer_.program_storage_inputs.empty()))
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {source_index}});

                CpuCall linked{
                    .call = std::move(*call),
                    .inputs = inputs_,
                    .inline_blocks = constants_,
                    .streams = {first_stream, static_cast<std::uint32_t>(streams.size())},
                    .buffers = {first_buffer, buffer_count},
                    .snapshots = snapshots_
                };
                if (dynamic)
                {
                    if (plan.raster_builds.size() == kGraphInvalidIndex) return cxx::unexpected(RenderError{kRuntimeCapacity});
                    linked.program_slot = static_cast<std::uint32_t>(plan.raster_builds.size());
                    auto& recipe = plan.raster_builds.emplace_back(static_cast<std::uint32_t>(plan.calls.size()));
                    if (!recipe.link(builder, linked.call))
                        return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {source_index}});
                    recipe.snapshots = {first_program_read, static_cast<std::uint32_t>(producer_.program_storage_inputs.size())};
                }
                if (linked.call.methods->create_workspace)
                {
                    if (plan.workspace_count == kGraphInvalidIndex)
                        return cxx::unexpected(RenderError{kRuntimeCapacity});
                    linked.workspace_slot = plan.workspace_count++;
                }
                if (!producer_.outputs.empty())
                {
                    // assignSlots lays out all outputs of one live call consecutively.
                    linked.outputs = {
                        workspace_.data_slots[producer_.outputs.front().index],
                        static_cast<std::uint32_t>(producer_.outputs.size())
                    };
                }
                plan.calls.push_back(std::move(linked));
                return {};
            }

        private:
            RenderResult<extension_detail::SnapshotOperand>
            resolveStorageSlot(GraphStorageSnapshotId id, const cxx::basic_type_info& type) const noexcept override
            {
                const auto found = std::ranges::find(producer_.storage_inputs, id);
                if (found == producer_.storage_inputs.end())
                    return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, id.index}});
                const auto& expected = workspace_.build.snapshots[id.index].type;
                if (expected != type || expected.name() != type.name())
                    return cxx::unexpected(RenderError{kStorageMismatch, {id.index}});
                const auto local = static_cast<std::uint32_t>(found - producer_.storage_inputs.begin());
                const auto operand = snapshots_.first + local;
                auto& read = workspace_.plan.snapshot_reads[operand];
                // The author supplies a static type object. Every successful resolution receives the same witness.
                if (!read.type) read.type = &type;
                return extension_detail::SnapshotOperand{operand, read.program, read.type};
            }

            RenderResult<std::uint32_t> claimEffectInput(cxx::basic_type_info type, std::size_t bytes) noexcept override
            {
                if (source_input_ == kGraphInvalidIndex)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {producer_slot_}});
                const auto& input = workspace_.effect_inputs[source_input_];
                if (input.type != type || input.type.name() != type.name() || input.bytes != bytes)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {producer_slot_}});
                auto& slot = workspace_.effect_slots[source_input_];
                if (slot == kGraphInvalidIndex)
                {
                    auto& plan = workspace_.plan;
                    slot = static_cast<std::uint32_t>(plan.cpu_values.size());
                    plan.cpu_values.push_back({bytes});
                    plan.effect_inputs.push_back({source_input_, {slot}});
                }
                if (selected_input_ == kGraphInvalidIndex)
                {
                    auto& reads = workspace_.plan.cpu_reads;
                    if (reads.size() == kGraphInvalidIndex) return cxx::unexpected(RenderError{kRuntimeCapacity});
                    selected_input_ = static_cast<std::uint32_t>(reads.size());
                    reads.push_back({slot});
                    ++inputs_.count;
                }
                return selected_input_;
            }

            RenderResult<DrawStreamSlot> resolveDrawStream(GraphPassId pass) noexcept override
            {
                auto resolved = streamPass(pass);
                if (!resolved) return cxx::unexpected(resolved.error());
                auto& ranges = workspace_.plan.draw_resource_ranges;
                if (ranges.empty()) ranges.resize(workspace_.compiled.graph.passes().size());
                if (ranges[*resolved].producer != kGraphInvalidIndex)
                    return cxx::unexpected(RenderError{kGraphAmbiguousProducer, {pass.index}});
                ranges[*resolved].producer = producer_slot_;
                const auto list = workspace_.compiled.graph.programs().rasterStream(*resolved)->list;
                ranges[*resolved].list = list;
                workspace_.plan.streams.push_back({*resolved, list});
                return DrawStreamSlot{*resolved};
            }

            RenderResult<extension_detail::DrawSourceAddress>
            resolveDrawSource(
                GraphPassId pass, std::string_view binding, cxx::basic_type_info type
            ) const noexcept override
            {
                const auto resolved = streamPass(pass);
                if (!resolved) return cxx::unexpected(resolved.error());
                const auto& ranges = workspace_.plan.draw_resource_ranges;
                if (*resolved < ranges.size())
                {
                    const auto range = ranges[*resolved];
                    for (std::uint32_t offset = 0; offset < range.count; ++offset)
                    {
                        const auto source = range.first + offset;
                        const auto slot = workspace_.plan.draw_resource_bindings[source];
                        const auto& declared = workspace_.build.storage_imports[slot.binding];
                        if (declared.binding != binding) continue;
                        const auto& draw = std::get<EffectStorageImport::DrawContract>(declared.contract);
                        if (draw.hash != type.hash() || draw.name != type.name())
                            return cxx::unexpected(RenderError{kStorageMismatch, {pass.index, slot.binding}});
                        return extension_detail::DrawSourceAddress{*resolved, source};
                    }
                }
                return cxx::unexpected(RenderError{kGraphUnknownResource, {pass.index}});
            }

            RenderResult<RasterProgramSlot>
            resolveRasterProgram(GraphPassId pass, std::uint32_t source_program) const noexcept override
            {
                auto resolved = streamPass(pass);
                if (!resolved) return cxx::unexpected(resolved.error());
                const auto& raster = std::get<RasterWork>(workspace_.build.source.passes()[pass.index].command);
                const auto& programs = std::get<RasterPassTemplate>(raster.program).stream_programs;
                const auto use = std::ranges::find(programs, source_program, &RasterPassTemplate::StreamProgram::program);
                if (use == programs.end()) return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {pass.index}});
                return RasterProgramSlot{*resolved, static_cast<std::uint32_t>(use - programs.begin())};
            }

            RenderResult<RasterBufferSlot>
            resolveRasterBuffer(GraphPassId pass, GraphResourceId source) const noexcept override
            {
                auto resolved = streamPass(pass);
                if (!resolved) return cxx::unexpected(resolved.error());
                const auto& buffers = workspace_.compiled.graph.programs().rasterStream(*resolved)->buffers;
                const auto use = std::ranges::find(buffers, source.index, &detail::CompiledRasterStream::BufferAccess::resource);
                if (use == buffers.end())
                    return cxx::unexpected(RenderError{kGraphUnknownResource, {pass.index, source.index}});
                return RasterBufferSlot{source.index};
            }

            RenderResult<RasterImageSlot>
            resolveRasterImage(GraphPassId pass, GraphResourceId source) const noexcept override
            {
                auto resolved = streamPass(pass);
                if (!resolved) return cxx::unexpected(resolved.error());
                const auto& images = workspace_.compiled.graph.programs().rasterStream(*resolved)->images;
                const auto use = std::ranges::find(images, source.index, &detail::CompiledRasterStream::ImageAccess::resource);
                if (use == images.end())
                    return cxx::unexpected(RenderError{kGraphUnknownResource, {pass.index, source.index}});
                return RasterImageSlot{source.index};
            }

            RenderResult<RasterSamplerSlot>
            resolveRasterSampler(GraphPassId pass, std::uint32_t source) const noexcept override
            {
                auto resolved = streamPass(pass);
                if (!resolved) return cxx::unexpected(resolved.error());
                const auto& raster = std::get<RasterWork>(workspace_.build.source.passes()[pass.index].command);
                const auto& samplers = std::get<RasterPassTemplate>(raster.program).stream_samplers;
                const auto use = std::ranges::find(samplers, source);
                if (use == samplers.end()) return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {pass.index}});
                return RasterSamplerSlot{*resolved, static_cast<std::uint32_t>(use - samplers.begin())};
            }

            RenderResult<std::uint32_t>
            resolveRasterArgument(GraphPassId pass, std::string_view name) const noexcept override
            {
                auto resolved = streamPass(pass);
                if (!resolved) return cxx::unexpected(resolved.error());
                const auto& raster = std::get<RasterWork>(workspace_.build.source.passes()[pass.index].command);
                const auto& inputs = std::get<RasterPassTemplate>(raster.program).inputs;
                const auto input = std::ranges::find(inputs, name, &RasterPassTemplate::Input::name);
                if (input == inputs.end() || !input->kind)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {pass.index}});
                return static_cast<std::uint32_t>(input - inputs.begin());
            }

            RenderResult<std::uint32_t> streamPass(GraphPassId pass) const noexcept
            {
                if (std::ranges::find(producer_.passes, pass) == producer_.passes.end())
                    return cxx::unexpected(RenderError{kGraphUnknownPass, {pass.index}});
                const auto slot = workspace_.pass_slots[pass.index].index;
                const bool live_stream = slot < workspace_.compiled.graph.passes().size() &&
                    workspace_.compiled.graph.programs().rasterStream(slot);
                if (!live_stream)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {pass.index}});
                return slot;
            }

            RenderResult<std::uint32_t>
            resolveCpuSlot(
                GraphCpuDataId id, cxx::basic_type_info type, std::size_t bytes, extension_detail::ECpuPort direction
            ) const noexcept override
            {
                const bool output = direction == extension_detail::ECpuPort::OUTPUT;
                const auto& ports = output ? producer_.outputs : producer_.inputs;
                const auto found = std::ranges::find(ports, id);
                if (found == ports.end()) return cxx::unexpected(RenderError{kGraphUnknownResource, {id.index}});
                const auto& data = workspace_.build.cpu_data[id.index];
                if (data.type != type || data.type.name() != type.name() || data.bytes != bytes)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {kGraphInvalidIndex, id.index}});
                if (output) return workspace_.data_slots[id.index];
                return inputs_.first + static_cast<std::uint32_t>(found - ports.begin());
            }

            RenderResult<std::uint32_t>
            claimInlineBlock(GraphPassId id, cxx::basic_type_info type, std::size_t bytes) noexcept override
            {
                if (std::ranges::find(producer_.inline_blocks, id) == producer_.inline_blocks.end())
                    return cxx::unexpected(RenderError{kGraphUnknownPass, {id.index}});
                const auto slots = workspace_.destinations(id);
                const auto slot = slots[0].index;
                const auto* parameters = workspace_.build.source.passes()[id.index].bindings().parameters;
                const bool valid_slot = slot < workspace_.compiled.graph.passes().size();
                const bool compatible = valid_slot &&
                    workspace_.compiled.graph.passes()[slot].parameter_size == bytes &&
                    parameters && parameters->inline_constant.type == type &&
                    parameters->inline_constant.type.name() == type.name();
                if (!compatible) return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {id.index}});
                auto& table = workspace_.plan.inline_blocks;
                const auto claimed = std::span{table}.subspan(constants_.first);
                if (std::ranges::find(claimed, slot, &detail::LinkedInlineBlock::pass) != claimed.end())
                    return cxx::unexpected(RenderError{kGraphAmbiguousProducer, {id.index}});
                if (table.size() == kGraphInvalidIndex) return cxx::unexpected(RenderError{kRuntimeCapacity});
                const auto writer = static_cast<std::uint32_t>(table.size());
                table.push_back({slot, {bytes}, slots[1].index});
                ++constants_.count;
                return writer;
            }

            RenderResult<std::uint32_t> claimPreparedBuffer(GraphResourceId resource, bool upload) noexcept override
            {
                const auto declared = std::ranges::find(producer_.gpu_buffers, resource);
                if (declared == producer_.gpu_buffers.end())
                    return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, resource.index}});
                const auto& buffer = workspace_.build.source.resources()[resource.index];
                if (upload != (buffer.origin == EGraphOrigin::IMPORTED))
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {kGraphInvalidIndex, resource.index}});
                const auto slot = std::exchange(workspace_.buffer_slots[resource.index], kGraphInvalidIndex);
                if (slot == kGraphInvalidIndex)
                    return cxx::unexpected(RenderError{kGraphAmbiguousProducer, {kGraphInvalidIndex, resource.index}});
                ++claimed_buffers_;
                return slot;
            }

            PreparationLinkWorkspace& workspace_;
            const GraphPreparation& producer_;
            detail::PreparationRange inputs_, constants_, snapshots_;
            std::uint32_t claimed_buffers_ = 0;
            std::uint32_t source_input_, selected_input_ = kGraphInvalidIndex;
            std::uint32_t producer_slot_;
        };

        RenderResult<void>
        linkPreparation(
            const EffectGraphBuild& build,
            const detail::PreparedRenderGraph& compiled,
            std::span<const EffectInputDefinition> inputs, CpuProgram& plan
        ) noexcept
        {
            const auto& definition = build;
            const auto& source = build.source;
            const auto& executable = compiled.graph;
            PreparationLinkWorkspace workspace(build, compiled, inputs, plan);
            for (std::size_t p = 0; p < source.passes().size(); ++p)
            {
                const auto slot = workspace.pass_slots[p].index;
                if (slot == kGraphInvalidIndex) continue;
                const auto* raster = std::get_if<RasterWork>(&source.passes()[p].command);
                const auto* stage = raster ? std::get_if<RasterPassTemplate>(&raster->program) : nullptr;
                if (!stage) continue;
                if (stage->draw_resources.empty()) continue;
                if (plan.draw_resource_ranges.empty()) plan.draw_resource_ranges.resize(executable.passes().size());
                const auto first = plan.draw_resource_bindings.size();
                if (stage->draw_resources.size() > kGraphInvalidIndex - first)
                    return cxx::unexpected(RenderError{kRuntimeCapacity});
                for (const auto& input : stage->draw_resources)
                {
                    const auto& imports = build.storage_imports;
                    const auto it = std::ranges::lower_bound(imports, input.binding, {}, &EffectStorageImport::binding);
                    if (it == imports.end() || it->binding != input.binding)
                        return cxx::unexpected(RenderError{kGraphUnknownResource, {p}});
                    plan.draw_resource_bindings.push_back({
                        static_cast<std::uint32_t>(it - imports.begin()), input.required_capabilities,
                        input.indexed_geometry
                    });
                }
                plan.draw_resource_ranges[slot] = {
                    static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(stage->draw_resources.size())
                };
            }
            for (const auto& copy : definition.inline_copies)
            {
                const auto slots = workspace.destinations(copy.pass);
                if (slots.front().index == kGraphInvalidIndex) continue;
                const auto state = std::ranges::find(inputs, copy.node, &EffectInputDefinition::node);
                if (state == inputs.end() || state->type != copy.input.state_type)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {copy.pass.index}});
                const auto source_index = static_cast<std::uint32_t>(state - inputs.begin());
                const auto found = std::ranges::find_if(plan.effect_inputs, [&](const auto& input) noexcept
                {
                    return input.source == source_index && input.bind == copy.input.bind;
                });
                auto input = static_cast<std::uint32_t>(plan.cpu_values.size());
                if (found != plan.effect_inputs.end()) input = found->destination.index;
                else
                {
                    plan.cpu_values.push_back({copy.input.block.bytes});
                    plan.effect_inputs.push_back({source_index, {input}, copy.input.bind});
                }
                for (auto pass : slots)
                    if (pass.index != kGraphInvalidIndex) plan.constant_copies.push_back({{input}, pass});
            }
            plan.calls.reserve(definition.live_order.size());
            for (auto index : definition.live_order)
            {
                const auto& [declaration, node] = definition.producers[index];
                const auto input = std::ranges::find(inputs, node, &EffectInputDefinition::node);
                const auto slot = input == inputs.end() ? kGraphInvalidIndex :
                    static_cast<std::uint32_t>(input - inputs.begin());
                LinkContext context{
                    workspace, declaration, slot,
                    static_cast<std::uint32_t>(plan.calls.size())
                };
                auto linked = context.link(index);
                if (!linked) return cxx::unexpected(linked.error());
            }
            for (std::uint32_t p = 0; p < compiled.graph.passes().size(); ++p)
            {
                const auto& grants = plan.draw_resource_ranges;
                if (compiled.graph.programs().rasterStream(p) && (grants.empty() || grants[p].producer == kGraphInvalidIndex))
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
            for (auto& block : plan.inline_blocks)
                if (!place(block.bytes)) return cxx::unexpected(RenderError{kRuntimeCapacity});
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
                    request.recipe, env.catalog, {request.output, device}, env.diagnostics
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
        auto preparation = linkPreparation(*built, *plan, request.inputs, plan->graph.cpu_program_);
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
