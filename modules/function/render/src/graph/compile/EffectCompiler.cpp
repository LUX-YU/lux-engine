#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/graph/execute/RasterStreamPlan.hpp>
#include <lux/engine/render/device/NativeResult.hpp>
#include <lux/engine/render/graph/RenderGraph.hpp>
#include <lux/engine/render/graph/Range.hpp>
#include <lux/engine/render/graph/compile/EffectCompileRequest.hpp>
#include <lux/engine/render/graph/compile/GraphCompiler.hpp>
#include <lux/engine/render/runtime/DeviceEntry.hpp>

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
            std::vector<std::uint8_t> claimed_streams;
            detail::PreparationPlan plan;

            PreparationLinkWorkspace(
                const EffectGraphBuild& graph, const detail::PreparedRenderGraph& native,
                std::span<const EffectInputDefinition> inputs
            ) noexcept
                : build(graph), compiled(native), data_slots(build.preparation.cpu_data.size(), kGraphInvalidIndex),
                  buffer_slots(build.source.resources().size(), kGraphInvalidIndex),
                  snapshot_slots(build.preparation.snapshots.size(), kGraphInvalidIndex),
                  effect_inputs(inputs), effect_slots(inputs.size(), kGraphInvalidIndex),
                  pass_slots(graph.source.passes().size()),
                  claimed_streams(compiled.executable.passes().size())
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

            bool claimStream(std::uint32_t slot) noexcept
            {
                return !std::exchange(claimed_streams[slot], std::uint8_t{1});
            }

        private:
            void assignSlots() noexcept
            {
                const auto& definition = build.preparation;
                for (std::uint32_t dense = 0; dense < definition.live_order.size(); ++dense)
                {
                    const auto producer = definition.live_order[dense];
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
                        data_slots[output.index] = static_cast<std::uint32_t>(plan.cpu_data_bytes.size());
                        plan.cpu_data_bytes.push_back(definition.cpu_data[output.index].bytes);
                    }
                    for (auto output : definition.producers[producer].declaration.gpu_buffers)
                    {
                        const auto buffer = std::ranges::find(
                            definition.buffers, output, &PreparationDefinition::Buffer::resource
                        );
                        buffer_slots[output.index] = static_cast<std::uint32_t>(plan.buffers.size());
                        plan.buffers.push_back({
                            output.index, buffer->minimum_bytes, buffer->maximum_bytes, buffer->upload,
                            compiled.executable.resources()[output.index].memory_used, dense
                        });
                    }
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

            RenderResult<detail::LinkedPreparationProducer> link(std::uint32_t source_index) noexcept
            {
                auto prototype = producer_.factory.link(producer_.configuration, *this);
                if (!prototype) return cxx::unexpected(prototype.error());
                const auto expected = std::ranges::count_if(producer_.inline_blocks, [&](GraphPassId pass) noexcept
                {
                    return workspace_.destinations(pass)[0].index != kGraphInvalidIndex;
                });
                const bool missing_constants = constants_.size() != static_cast<std::size_t>(expected);
                const bool missing_buffers = buffers_.size() != producer_.gpu_buffers.size();
                if (missing_constants || missing_buffers)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {source_index}});

                detail::LinkedPreparationProducer linked{
                    .prototype = std::move(*prototype),
                    .inline_blocks = std::move(constants_),
                    .raster_streams = std::move(streams_),
                    .buffers = std::move(buffers_)
                };
                for (auto id : producer_.inputs) linked.inputs.push_back(workspace_.data_slots[id.index]);
                if (selected_input_ != kGraphInvalidIndex) linked.inputs.push_back(selected_input_);
                for (auto id : producer_.outputs) linked.outputs.push_back(workspace_.data_slots[id.index]);
                for (auto id : producer_.storage_inputs)
                    linked.snapshots.push_back(workspace_.snapshot_slots[id.index]);
                for (auto id : producer_.program_storage_inputs)
                    linked.program_snapshots.push_back(workspace_.snapshot_slots[id.index]);
                return linked;
            }

        private:
            RenderResult<std::uint32_t>
            resolveStorageSlot(GraphStorageSnapshotId id, cxx::basic_type_info type) const noexcept override
            {
                const bool declared = std::ranges::find(producer_.storage_inputs, id) != producer_.storage_inputs.end();
                if (!declared)
                    return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, id.index}});
                const auto expected = workspace_.build.preparation.snapshots[id.index].type;
                if (expected != type || expected.name() != type.name())
                    return cxx::unexpected(RenderError{kStorageMismatch, {id.index}});
                return workspace_.snapshot_slots[id.index];
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
                    slot = static_cast<std::uint32_t>(plan.cpu_data_bytes.size());
                    plan.cpu_data_bytes.push_back(bytes);
                    plan.effect_inputs.push_back({source_input_, {slot}});
                }
                selected_input_ = slot;
                return slot;
            }

            RenderResult<DrawStreamSlot> resolveDrawStream(GraphPassId pass) noexcept override
            {
                auto resolved = streamPass(pass);
                if (!resolved) return cxx::unexpected(resolved.error());
                if (!workspace_.claimStream(*resolved))
                    return cxx::unexpected(RenderError{kGraphAmbiguousProducer, {pass.index}});
                streams_.push_back(*resolved);
                return DrawStreamSlot{*resolved};
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
                const auto& buffers = workspace_.compiled.programs.rasterStream(*resolved)->buffers;
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
                const auto& images = workspace_.compiled.programs.rasterStream(*resolved)->images;
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
                const bool live_stream = slot < workspace_.compiled.executable.passes().size() &&
                    workspace_.compiled.programs.rasterStream(slot);
                if (!live_stream)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {pass.index}});
                return slot;
            }

            RenderResult<std::uint32_t>
            resolveCpuSlot(GraphCpuDataId id, cxx::basic_type_info type, std::size_t bytes) const noexcept override
            {
                const bool declared = std::ranges::find(producer_.inputs, id) != producer_.inputs.end() ||
                    std::ranges::find(producer_.outputs, id) != producer_.outputs.end();
                if (!declared) return cxx::unexpected(RenderError{kGraphUnknownResource, {id.index}});
                const auto& data = workspace_.build.preparation.cpu_data[id.index];
                if (data.type != type || data.bytes != bytes)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {kGraphInvalidIndex, id.index}});
                return workspace_.data_slots[id.index];
            }

            RenderResult<std::uint32_t>
            claimInlineBlock(GraphPassId id, cxx::basic_type_info type, std::size_t bytes) noexcept override
            {
                if (std::ranges::find(producer_.inline_blocks, id) == producer_.inline_blocks.end())
                    return cxx::unexpected(RenderError{kGraphUnknownPass, {id.index}});
                const auto slots = workspace_.destinations(id);
                const auto slot = slots[0].index;
                const auto* parameters = workspace_.build.source.passes()[id.index].bindings().parameters;
                const bool valid_slot = slot < workspace_.compiled.executable.passes().size();
                const bool compatible = valid_slot &&
                    workspace_.compiled.executable.passes()[slot].parameter_size == bytes &&
                    parameters && parameters->inline_constant.type == type;
                if (!compatible) return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {id.index}});
                const auto previous = std::ranges::find(constants_, slot, &detail::LinkedInlineBlock::pass);
                if (previous != constants_.end())
                    return cxx::unexpected(RenderError{kGraphAmbiguousProducer, {id.index}});
                constants_.push_back({slot, bytes, slots[1].index});
                return slot;
            }

            RenderResult<std::uint32_t> claimPreparedBuffer(GraphResourceId resource, bool upload) noexcept override
            {
                const auto declared = std::ranges::find(producer_.gpu_buffers, resource);
                if (declared == producer_.gpu_buffers.end())
                    return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, resource.index}});
                const auto& buffer = workspace_.build.source.resources()[resource.index];
                if (upload != (buffer.origin == EGraphOrigin::IMPORTED))
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {kGraphInvalidIndex, resource.index}});
                const auto slot = workspace_.buffer_slots[resource.index];
                if (slot == kGraphInvalidIndex) std::terminate();
                if (std::ranges::find(buffers_, slot) != buffers_.end())
                    return cxx::unexpected(RenderError{kGraphAmbiguousProducer, {kGraphInvalidIndex, resource.index}});
                buffers_.push_back(slot);
                return slot;
            }

            PreparationLinkWorkspace& workspace_;
            const GraphPreparation& producer_;
            std::vector<detail::LinkedInlineBlock> constants_;
            std::vector<std::uint32_t> buffers_, streams_;
            std::uint32_t source_input_, selected_input_ = kGraphInvalidIndex;
            std::uint32_t producer_slot_;
        };

        RenderResult<detail::PreparationPlan>
        linkPreparation(
            const EffectGraphBuild& build,
            const detail::PreparedRenderGraph& compiled,
            std::span<const EffectInputDefinition> inputs
        ) noexcept
        {
            const auto& definition = build.preparation;
            const auto& source = build.source;
            const auto& executable = compiled.executable;
            PreparationLinkWorkspace workspace(build, compiled, inputs);
            auto& plan = workspace.plan;
            for (std::size_t p = 0; p < source.passes().size(); ++p)
            {
                const auto slot = workspace.pass_slots[p].index;
                if (slot == kGraphInvalidIndex) continue;
                const auto* raster = std::get_if<RasterWork>(&source.passes()[p].command);
                const auto* stage = raster ? std::get_if<RasterPassTemplate>(&raster->program) : nullptr;
                if (!stage) continue;
                if (!stage->shared_textures && !stage->mesh_geometry && !stage->shared_materials) continue;
                if (plan.raster_bindings.empty()) plan.raster_bindings.resize(executable.passes().size());
                auto& bindings = plan.raster_bindings[slot];
                if (stage->shared_textures) bindings.textures = definition.texture_sets[stage->shared_textures->index];
                if (stage->mesh_geometry) bindings.meshes = definition.mesh_sets[stage->mesh_geometry->index];
                if (stage->shared_materials) bindings.materials = definition.material_sets[stage->shared_materials->index];
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
                auto input = static_cast<std::uint32_t>(plan.cpu_data_bytes.size());
                if (found != plan.effect_inputs.end()) input = found->destination.index;
                else
                {
                    plan.cpu_data_bytes.push_back(copy.input.block.bytes);
                    plan.effect_inputs.push_back({source_index, {input}, copy.input.bind});
                }
                for (auto pass : slots)
                    if (pass.index != kGraphInvalidIndex) plan.constant_copies.push_back({{input}, pass});
            }
            plan.producers.reserve(definition.live_order.size());
            for (auto index : definition.live_order)
            {
                const auto& [declaration, node] = definition.producers[index];
                const auto input = std::ranges::find(inputs, node, &EffectInputDefinition::node);
                const auto slot = input == inputs.end() ? kGraphInvalidIndex :
                    static_cast<std::uint32_t>(input - inputs.begin());
                LinkContext context{
                    workspace, declaration, slot,
                    static_cast<std::uint32_t>(plan.producers.size())
                };
                auto linked = context.link(index);
                if (!linked) return cxx::unexpected(linked.error());
                plan.producers.push_back(std::move(*linked));
            }
            for (std::uint32_t p = 0; p < workspace.claimed_streams.size(); ++p)
            {
                if (compiled.programs.rasterStream(p) && !workspace.claimed_streams[p])
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {p}});
            }
            return std::move(workspace.plan);
        }
        struct LinkedBufferOutputs final
        {
            std::vector<FinalBufferUse> uses;
            std::vector<FinalBufferExport> exports;
        };

        RenderResult<LinkedBufferOutputs> linkBufferOutputs(
            EffectGraphBuild& build, const RenderGraphExecutable& executable, DeviceSession& session
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
                    owner[resource] = i;
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
                auto& queue = slots[binding].slot.canonical_queue;
                const auto physical = session.queueIndex(session.queue(executable.passes()[pass].queue));
                const bool same_queue = queue == kGraphInvalidIndex || queue == physical;
                preserves_boundary[binding] = same_queue && use.usage == build.source.resources()[id.index].initial_use;
                queue = preserves_boundary[binding] ? static_cast<std::uint32_t>(physical) : kGraphInvalidIndex;
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
        DeviceEntry& entry, EffectCompileRequest request, EffectCompileEnvironment env
    ) noexcept
    {
        const auto& session = entry.session();
        const auto& features = session.enabledFeatures();
        const auto& limits = session.description();
        const auto& dispatch = limits.shaderStage.compute.dispatchMaxDim;
        const EffectBuildDevice device{
            features.shader_target, session.usesDeviceAddresses(), features.indirect_first_instance,
            {dispatch[0], dispatch[1], dispatch[2]}, limits.memory.storageBufferMaxRange,
            limits.dimensions.textureLayerMaxNum, features.texture_descriptors
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
        if (env.stopping.load()) co_return cxx::unexpected(RenderError{kCommandCancelled});
        if (const auto failure = entry.admissionError(); failure.type) co_return cxx::unexpected(failure);

        std::vector<GraphResourceId> concurrent_reads;
        for (std::size_t i = 0; i < built->storage_imports.size(); ++i)
        {
            const auto resource = built->storage_imports[i].slot.resource;
            if (resource.index == kGraphInvalidIndex) continue;
            const auto* storage = std::get_if<StorageGraphImport>(&built->storage_imports[i].contract);
            if (storage && storage->concurrent_image_read) concurrent_reads.push_back(resource);
        }
        auto plan = co_await compile(
            entry.session(), env.cpu, {built->source, built->analysis, concurrent_reads}, {.target = built->output}
        );
        if (!plan) co_return cxx::unexpected(plan.error());
        if (env.stopping.load()) co_return cxx::unexpected(RenderError{kCommandCancelled});
        if (const auto failure = entry.admissionError(); failure.type) co_return cxx::unexpected(failure);

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
            const auto& resource = plan->executable.resources()[slot.index];
            if (!resource.memory_used) slot = {};
            else if (!resource.sampled_target_input)
                co_return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {slot.index}});
        }
        for (auto& declaration : built->storage_imports)
        {
            auto& slot = declaration.slot;
            const auto id = slot.resource.index;
            if (id != kGraphInvalidIndex && !plan->executable.resources()[id].memory_used) slot = {};
        }
        auto preparation = linkPreparation(*built, *plan, request.inputs);
        if (!preparation) co_return cxx::unexpected(preparation.error());
        std::vector<detail::LinkedUniformBlock> uniforms;
        std::vector<detail::UniformPoolId> created_pools;
        cxx::scope_exit rollback_pools([&]() noexcept
        {
            for (const auto pool : created_pools) entry.eraseUniformPool(pool);
        });
        for (const auto& block : built->uniforms)
        {
            if (!plan->executable.resources()[block.resource.index].memory_used) continue;
            if (!entry.uploader()) co_return cxx::unexpected(RenderError{kDeviceUnsupported});
            const auto state = std::ranges::find(request.inputs, block.node, &EffectInputDefinition::node);
            if (state == request.inputs.end() || state->type != block.source.state_type)
                co_return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {block.node.value, block.resource.index}});
            auto pool = entry.findUniformPool(request.effect, block.node, block.name);
            if (!pool.isNull() && !entry.uniformPool(pool).versions.matches(block.source))
                co_return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {block.node.value, block.resource.index}});
            if (pool.isNull())
            {
                pool = entry.createUniformPool(request.effect, block.node, block.name, block.source);
                created_pools.push_back(pool);
            }

            auto& linked = uniforms.emplace_back(pool, block.resource, std::vector<std::uint32_t>{});
            for (const auto& access : plan->executable.importedAccesses())
            {
                if (access.resource != block.resource) continue;
                linked.batches.push_back(access.batch);
                linked.batches.push_back(access.release_batch);
            }
            std::ranges::sort(linked.batches);
            const auto duplicates = std::ranges::unique(linked.batches);
            linked.batches.erase(duplicates.begin(), duplicates.end());
        }
        if (env.stopping.load()) co_return cxx::unexpected(RenderError{kCommandCancelled});
        if (const auto failure = entry.admissionError(); failure.type) co_return cxx::unexpected(failure);
        auto outputs = linkBufferOutputs(*built, plan->executable, entry.session());
        if (!outputs) co_return cxx::unexpected(outputs.error());
        rollback_pools.release();
        co_return std::make_unique<const compiled::RenderGraph>(compiled::RenderGraph{
            std::move(plan->executable), std::move(plan->programs),
            std::move(built->target_imports), std::move(built->storage_imports), std::move(outputs->uses),
            std::move(outputs->exports),
            std::move(comparisons),
            std::move(*preparation),
            std::move(uniforms)
        });
    }
} // namespace lux::render::detail
