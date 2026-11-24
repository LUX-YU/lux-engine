#include <lux/engine/render/graph/compile/EffectDeclarations.hpp>
#include <lux/engine/render/effect/EffectValidation.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/SubmissionQueue.hpp>
#include <lux/engine/render/graph/execute/RasterStreamPlan.hpp>
#include <lux/engine/render/device/NativeResult.hpp>
#include <lux/engine/render/graph/RenderGraph.hpp>
#include <lux/engine/render/graph/Range.hpp>
#include <lux/engine/render/graph/compile/RenderGraphCompiler.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/runtime/commands/Error.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <lux/engine/render/frame/FrameError.hpp>
#include <tuple>

namespace lux::render
{
    RasterOperands detail::CompiledRasterStream::operands(
        GraphPassId pass, DrawStreamSlot stream, const RasterPassTemplate& layout, std::uint32_t first_source
    ) const & noexcept
    {
        return {{pass, stream}, layout, *this, first_source};
    }

    RenderResult<RasterResourceSlot>
    RasterOperands::resource(std::string_view name, ResourceNameView type) const noexcept
    {
        const auto& inputs = layout_.draw_resources;
        const auto found = std::ranges::find(inputs, name, &DrawResourceInput::binding);
        if (found == inputs.end()) return cxx::unexpected(RenderError{kGraphUnknownResource, {pass_.source.index}});
        if (found->source.name() != type.name())
            return cxx::unexpected(RenderError{kStorageMismatch, {pass_.source.index}});
        const auto source = first_source_ + static_cast<std::uint32_t>(found - inputs.begin());
        return RasterResourceSlot{source};
    }

    RenderResult<RasterProgramSlot> RasterOperands::programAt(std::uint32_t stream_index) const noexcept
    {
        if (stream_index >= layout_.fixedPrograms().size())
            return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {pass_.source.index, stream_index}});
        return RasterProgramSlot{pass_.compiled.index, stream_index};
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
        RenderResult<const void*> effectState(const effect_detail::ValidatedEffectNode& node, cxx::basic_type_info type) noexcept
        {
            const auto state = node.configured->state();
            if (!state.object || state.type != type || state.type.name() != type.name())
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {node.source.value}});
            return state.object;
        }

        // Cold link of surviving algorithm operands and inline copies to one compiled device graph.
        class FrameAlgorithmLink final
        {
        public:
            FrameAlgorithmLink(
                EffectDeclarations& declarations,
                AnalyzedGraph graph,
                const compiled::RenderGraph& compiled,
                EffectDeviceGraph& result
            ) noexcept
                : declarations_(declarations), source_(graph.graph), compiled_(compiled), result_(result),
                  pass_slots_(source_.passes().size())
            {
                const auto& order = graph.analysis.execution_order;
                for (std::uint32_t slot = 0; slot < order.size(); ++slot) pass_slots_[order[slot].index] = {slot};
            }

            RenderResult<void> link(rdesc::EShaderTarget target, std::vector<DynamicBuffer>& buffers) noexcept
            {
                for (const auto& copy : declarations_.inline_copies)
                {
                    const auto slots = destinations(copy.pass);
                    if (slots.front().index == kGraphInvalidIndex) continue;
                    auto state = effectState(*copy.node, copy.input.state_type);
                    if (!state) return cxx::unexpected(state.error());
                    const auto bound = copy.input.bind(*state);
                    for (auto pass : slots)
                    {
                        if (pass.index == kGraphInvalidIndex) continue;
                        const auto bytes = compiled_.passes()[pass.index].parameter_size;
                        if (!bytes || bytes != copy.input.block.bytes)
                            return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {copy.pass.index}});
                        if (bound.size() != bytes)
                            return cxx::unexpected(RenderError{kFrameContract, {pass.index}});
                        result_.inline_blocks.push_back({pass, bound});
                    }
                }
                result_.algorithms.calls.reserve(declarations_.algorithms.size());
                for (auto& algorithm : declarations_.algorithms)
                {
                    auto linked = linkAlgorithm(algorithm, target, buffers);
                    if (!linked) return cxx::unexpected(linked.error());
                }
                return {};
            }

        private:
            std::array<CompiledPassSlot, 2> destinations(GraphPassId id) const noexcept
            {
                std::array<CompiledPassSlot, 2> slots{pass_slots_[id.index], CompiledPassSlot{}};
                const auto peer = source_.passes()[id.index].choice_peer;
                if (peer.index != kGraphInvalidIndex) slots[1] = pass_slots_[peer.index];
                if (slots[0].index == kGraphInvalidIndex) std::swap(slots[0], slots[1]);
                return slots;
            }

            RenderResult<void>
            copyInline(
                GraphPassId pass, std::size_t offset, std::uint32_t bytes, std::uint32_t revision
            ) noexcept
            {
                for (auto destination : destinations(pass))
                {
                    if (destination.index == kGraphInvalidIndex) continue;
                    if (!bytes || compiled_.passes()[destination.index].parameter_size != bytes)
                        return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {pass.index}});
                    result_.algorithms.constant_copies.push_back({offset, destination, bytes, revision});
                }
                return {};
            }

            RenderResult<void> linkAlgorithm(
                EffectDeclarations::Algorithm& algorithm, rdesc::EShaderTarget target, std::vector<DynamicBuffer>& buffers
            ) noexcept
            {
                auto& plan = result_.algorithms;
                if (algorithm.declaration.resource_views.size() > kGraphInvalidIndex - plan.resource_view_slots.size())
                    return cxx::unexpected(RenderError{kRuntimeCapacity});
                const auto first_view = static_cast<std::uint32_t>(plan.resource_view_slots.size());
                const auto first_stream = static_cast<std::uint32_t>(plan.draw_outputs.size());
                streams_.clear();
                for (auto id : algorithm.declaration.draw_streams)
                    if (auto linked = linkStream(id); !linked) return linked;
                auto call = algorithm.bind(std::move(algorithm.configuration), streams_);
                if (!call) return cxx::unexpected(call.error());
                if (auto inputs = linkInputs(call->methods->inputs, algorithm); !inputs) return inputs;
                const auto inline_offset = plan.inline_bytes;
                if (auto outputs = linkResults(call->methods->outputs, algorithm, target); !outputs) return outputs;

                const auto stream_count = static_cast<std::uint32_t>(plan.draw_outputs.size()) - first_stream;
                const auto first_buffer = static_cast<std::uint32_t>(buffers.size());
                for (const auto outputs : {std::span{algorithm.declaration.uploads}, std::span{algorithm.declaration.frame_sizes}})
                {
                    for (auto id : outputs)
                    {
                        const auto& buffer = *declarations_.findBuffer(id);
                        const bool used = compiled_.resources()[id.index].memory_used;
                        buffers.push_back({
                            id.index, buffer.minimum_bytes, buffer.maximum_bytes, buffer.upload, used
                        });
                    }
                }
                FrameAlgorithm linked{
                    .call = std::move(*call),
                    .streams = {first_stream, stream_count},
                    .buffers = {first_buffer, static_cast<std::uint32_t>(buffers.size()) - first_buffer},
                    .first_view = first_view,
                    .inline_offset = inline_offset,
                    .inline_size = plan.inline_bytes - inline_offset
                };
                if (linked.call.methods->create_workspace)
                {
                    if (plan.workspace_count == kGraphInvalidIndex)
                        return cxx::unexpected(RenderError{kRuntimeCapacity});
                    linked.workspace_slot = plan.workspace_count++;
                }
                plan.calls.push_back(std::move(linked));
                return {};
            }

            RenderResult<void> linkStream(GraphPassId pass) noexcept
            {
                const auto slot = pass_slots_[pass.index].index;
                const auto& graph = compiled_;
                const auto streams = graph.programs().rasterStreams();
                const auto stream = std::ranges::lower_bound(streams, slot, {}, &CompiledRasterStream::pass);
                if (stream == streams.end() || stream->pass != slot)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {pass.index}});
                if (stream->resource_list) return cxx::unexpected(RenderError{kGraphAmbiguousProducer, {pass.index}});
                auto& plan = result_.algorithms;
                if (plan.output_by_pass.empty()) plan.output_by_pass.resize(graph.passes().size(), kGraphInvalidIndex);
                auto& output = plan.output_by_pass[slot];
                if (output != kGraphInvalidIndex)
                    return cxx::unexpected(RenderError{kGraphAmbiguousProducer, {pass.index}});
                const auto& work = std::get<RasterWork>(source_.passes()[pass.index].command);
                const auto& layout = std::get<RasterPassTemplate>(work.program);
                const auto first = plan.draw_resource_bindings.size();
                if (layout.draw_resources.size() > kGraphInvalidIndex - first)
                    return cxx::unexpected(RenderError{kRuntimeCapacity});
                output = static_cast<std::uint32_t>(plan.draw_outputs.size());
                for (const auto& input : layout.draw_resources)
                {
                    const auto& imports = declarations_.operands;
                    const auto it = std::ranges::lower_bound(imports, input.binding, {}, &EffectResourceOperand::binding);
                    if (it == imports.end() || it->binding != input.binding)
                        return cxx::unexpected(RenderError{kGraphUnknownResource, {pass.index}});
                    plan.draw_resource_bindings.push_back({
                        static_cast<std::uint32_t>(it - imports.begin()), output, input.reads
                    });
                }
                plan.draw_outputs.push_back(&*stream);
                streams_.push_back(stream->operands(pass, {slot}, layout, static_cast<std::uint32_t>(first)));
                return {};
            }

            RenderResult<void> linkInputs(std::span<const cxx::basic_type_info> types, const EffectDeclarations::Algorithm& algorithm) noexcept
            {
                if (types.size() != algorithm.declaration.resource_views.size())
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {algorithm.source_index}});
                for (std::size_t i = 0; i < types.size(); ++i)
                {
                    const auto id = algorithm.declaration.resource_views[i].index;
                    const auto& view = std::get<EffectResourceOperand::ViewContract>(declarations_.operands[id].contract);
                    if (types[i].hash() != view.hash || types[i].name() != view.name)
                        return cxx::unexpected(RenderError{kStorageMismatch, {algorithm.source_index, id}});
                    result_.algorithms.resource_view_slots.push_back(id);
                }
                return {};
            }

            RenderResult<void>
            linkResults(
                std::span<const extension_detail::InlineResultType> results,
                const EffectDeclarations::Algorithm& algorithm, rdesc::EShaderTarget target
            ) noexcept
            {
                if (results.size() != algorithm.declaration.inline_blocks.size())
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {algorithm.source_index}});
                auto& total = result_.algorithms.inline_bytes;
                for (std::size_t i = 0; i < results.size(); ++i)
                {
                    const auto& result = results[i];
                    if (result.bytes > std::vector<std::byte>{}.max_size() - total)
                        return cxx::unexpected(RenderError{kRuntimeCapacity});
                    const auto pass = algorithm.declaration.inline_blocks[i];
                    const auto* parameters = source_.passes()[pass.index].bindings().parameters;
                    const bool compatible = parameters &&
                        sameBlockContract(parameters->inline_constant, *result.block, target);
                    if (!compatible) return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {pass.index}});
                    auto copy = copyInline(pass, total, result.block->bytes, result.revision_offset);
                    total += result.bytes;
                    if (!copy) return copy;
                }
                return {};
            }

            EffectDeclarations& declarations_;
            const GraphDefinition& source_;
            const compiled::RenderGraph& compiled_;
            EffectDeviceGraph& result_;
            std::vector<CompiledPassSlot> pass_slots_;
            std::vector<RasterOperands> streams_;
        };

        void compactBufferWrites(std::vector<FinalBufferWrite>& writes) noexcept
        {
            if (writes.size() < 2) return;
            // Only writes with the same native receipt and frozen condition become effective together.
            std::ranges::sort(writes, {}, [](const auto& write) noexcept
            {
                return std::tuple{write.batch, write.condition_region, write.bytes.offset};
            });
            std::size_t count = 1;
            for (const auto& write : std::span{writes}.subspan(1))
            {
                auto& previous = writes[count - 1];
                const auto end = previous.bytes.offset + previous.bytes.size;
                const bool same_path = previous.batch == write.batch &&
                    previous.condition_region == write.condition_region;
                if (same_path && write.bytes.offset <= end)
                {
                    previous.bytes.size = std::max(end, write.bytes.offset + write.bytes.size) - previous.bytes.offset;
                }
                else writes[count++] = write;
            }
            writes.resize(count);
            // Export coverage walks the union in byte order across all enabled paths.
            std::ranges::sort(writes, {}, [](const auto& write) noexcept { return write.bytes.offset; });
        }

        bool linkExportCoverage(FinalBufferExport& output, std::span<const FinalBufferWrite> writes) noexcept
        {
            const auto end = output.bytes.offset + output.bytes.size;
            const auto first = std::ranges::find_if(writes, [&](const auto& write) noexcept
            {
                return write.bytes.offset + write.bytes.size > output.bytes.offset;
            });
            const auto after = std::ranges::lower_bound(first, writes.end(), end, {}, [](const auto& write) noexcept
            {
                return write.bytes.offset;
            });
            const std::span overlap{first, after};
            const auto unconditional = [](auto region) noexcept { return region == kGraphInvalidIndex; };
            if (bufferWritesCover(output.bytes, overlap, unconditional)) return true;
            if (!bufferWritesCover(output.bytes, overlap, [](auto) noexcept { return true; })) return false;
            output.conditional_writes = overlap;
            return true;
        }

        RenderResult<void> linkBufferOutputs(
            EffectDeclarations& declarations, AnalyzedGraph source, const compiled::RenderGraph& executable, DeviceSession& session
        ) noexcept
        {
            auto& slots = declarations.operands;
            auto& manifest = declarations.resources;
            if (slots.empty()) return {};
            std::vector<std::uint32_t> owner(source.graph.resources().size(), kGraphInvalidIndex);
            for (std::uint32_t i = 0; i < slots.size(); ++i)
            {
                const auto resource = slots[i].slot.resource.index;
                if (resource == kGraphInvalidIndex) continue;
                if (std::holds_alternative<GraphBufferDesc>(source.graph.resources()[resource].description))
                {
                    owner[resource] = i;
                    slots[i].slot.boundary.use = source.graph.resources()[resource].initial_use;
                }
            }
            struct BufferOutputDraft final
            {
                std::vector<FinalBufferWrite> writes;
                std::vector<FinalBufferExport> exports;
                std::vector<std::uint32_t> binding_regions;
                bool preserves_boundary = true;
            };
            std::vector<BufferOutputDraft> drafts(slots.size());
            const auto observe = [&](GraphResourceId id, std::uint32_t pass, const ResourceAccess& use, bool write)
            {
                const auto binding = owner[id.index];
                if (binding == kGraphInvalidIndex) return;
                const auto& command = executable.passes()[pass];
                auto& slot = slots[binding].slot;
                auto& draft = drafts[binding];
                if (slot.output_range)
                {
                    const auto region = command.condition_region;
                    if (write)
                        draft.writes.push_back({std::get<GraphBufferRange>(use.range), command.submission, region});
                    if (!slot.unconditional_access)
                    {
                        auto& regions = draft.binding_regions;
                        const bool fixed_at_install = region != kGraphInvalidIndex &&
                            executable.recordingProgram().conditions[region].source == EConditionSource::SAME_BINDING;
                        if (!fixed_at_install)
                        {
                            slot.unconditional_access = true;
                            regions.clear();
                        }
                        else if (regions.empty() || regions.back() != region) regions.push_back(region);
                    }
                }
                if (!draft.preserves_boundary) return;
                auto& boundary = slot.boundary;
                const auto physical = session.queueIndex(session.queue(command.queue));
                const bool same_queue = boundary.queue == kGraphInvalidIndex || boundary.queue == physical;
                draft.preserves_boundary = same_queue && use.usage == boundary.use;
                boundary.queue = draft.preserves_boundary ? static_cast<std::uint32_t>(physical) : kGraphInvalidIndex;
            };
            const auto& order = source.analysis.execution_order;
            for (std::uint32_t p = 0; p < order.size(); ++p)
            {
                for (const auto& use : source.graph.passes()[order[p].index].uses)
                {
                    observe(use.version.resource, p, use, writes(use.access));
                    if (use.fallback) observe(use.fallback->resource, p, use, false);
                }
            }
            for (const auto& output : source.graph.outputs())
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
                        drafts[binding].exports.push_back({std::get<GraphBufferRange>(output.range), guard, {}});
                };
                append(output.version);
                if (output.alternate)
                {
                    guard->expected = !guard->expected;
                    append(output.alternate->value);
                }
            }
            std::size_t region_count = 0, write_count = 0, export_count = 0;
            for (auto& draft : drafts)
            {
                compactBufferWrites(draft.writes);
                region_count += draft.binding_regions.size();
                write_count += draft.writes.size();
                export_count += draft.exports.size();
            }
            // Barrier-only accesses can touch a queue even when the command is conditionally skipped.
            for (const auto& pass : executable.passes())
            {
                const auto barriers = executable.recordingProgram().bufferBarriers(pass);
                if (barriers.empty()) continue;
                const auto queue = session.queueIndex(session.queue(pass.queue));
                for (const auto& barrier : barriers)
                {
                    const auto i = owner[barrier.resource];
                    if (i != kGraphInvalidIndex && slots[i].slot.boundary.queue != queue)
                        slots[i].slot.boundary.queue = kGraphInvalidIndex;
                }
            }
            manifest.buffer_binding_regions.reserve(region_count);
            manifest.buffer_writes.reserve(write_count);
            manifest.buffer_exports.reserve(export_count);
            for (std::size_t i = 0; i < slots.size(); ++i)
            {
                auto& binding = slots[i].slot;
                if (binding.boundary.queue != kGraphInvalidIndex)
                {
                    const auto& resource = executable.resources()[binding.resource.index];
                    const auto queue_of = [&](EQueueRole role) { return session.queueIndex(session.queue(role)); };
                    const bool foreign_restore = !resource.exit_barrier_uses.empty() &&
                        queue_of(executable.submissions().back().queue) != binding.boundary.queue;
                    if (foreign_restore) binding.boundary.queue = kGraphInvalidIndex;
                }
                // Publication checks accepted writes in byte order. Reserve the complete tables
                // before publishing spans so later operands cannot invalidate an earlier slot.
                auto& regions = manifest.buffer_binding_regions;
                const auto first_region = regions.size();
                auto& draft = drafts[i];
                regions.insert(regions.end(), draft.binding_regions.begin(), draft.binding_regions.end());
                binding.binding_regions = std::span{regions}.subspan(first_region);
                const auto& writes = draft.writes;
                const auto first_write = manifest.buffer_writes.size();
                manifest.buffer_writes.insert(manifest.buffer_writes.end(), writes.begin(), writes.end());
                binding.writes = std::span{manifest.buffer_writes}.subspan(first_write);
                const auto first_export = manifest.buffer_exports.size();
                for (auto& exported : draft.exports)
                {
                    if (linkExportCoverage(exported, binding.writes))
                    {
                        manifest.buffer_exports.push_back(exported);
                    }
                }
                binding.exports = std::span{manifest.buffer_exports}.subspan(first_export);
            }
            return {};
        }

        error::ErrorId operandFactoryError(
            const EffectResourceOperand& declaration, const ResourceRegistration& factory,
            GpuResourceFeatures available
        ) noexcept
        {
            if (factory.descriptor->type.name() != declaration.type.name())
                return kStorageMismatch;
            const auto* ops = factory.storage().bindings();
            if (const auto* reads = std::get_if<DrawResourceReads>(&declaration.contract))
            {
                const bool missing_images = reads->containsAny(EDrawResourceRead::SAMPLED_IMAGES) &&
                    !available.containsAny(EGpuResourceFeature::SAMPLED_IMAGE_ARRAY);
                const bool missing_records = reads->containsAny(EDrawResourceRead::STORAGE_RECORDS) &&
                    !available.containsAny(EGpuResourceFeature::STORAGE_BUFFER_ARRAY);
                if (missing_images || missing_records)
                    return kDeviceUnsupported;
            }
            else if (const auto* expected = std::get_if<EffectResourceOperand::ViewContract>(&declaration.contract))
            {
                if (!ops || !ops->cpu_view) return kStorageMismatch;
                const auto& type = ops->cpu_view->type;
                if (type.hash() != expected->hash || type.name() != expected->name)
                    return kStorageMismatch;
            }
            else if (!ops || !ops->resolveGraphImport || !ops->describeGraphImport)
                return kStorageContract;
            return 0;
        }

        RenderResult<void> linkResourceManifest(
            EffectDeclarations& declarations, std::span<const std::uint32_t> used,
            const ResourceRegistry& registry, GpuResourceFeatures features
        ) noexcept
        {
            auto& manifest = declarations.resources;
            manifest.requirements.reserve(used.size());
            for (const auto index : used)
            {
                auto& declaration = declarations.requirements[index];
                const auto type = registry.typeId(ResourceNameView{declaration.type.name()});
                auto factory = registry.qualify(type, declaration.domain, features);
                if (!factory) return cxx::unexpected(factory.error());
                if ((*factory)->descriptor->lifetime != declaration.lifetime)
                    return cxx::unexpected(RenderError{kStorageMismatch});
                auto key = declaration.scope_default ? std::string{} : std::move(declaration.binding);
                manifest.requirements.push_back({type, *factory, std::move(key)});
            }
            const auto ops = [&](std::uint32_t slot) noexcept
            {
                return manifest.requirements[slot].factory->storage().bindings();
            };
            for (const auto& source : manifest.raster_sources)
            {
                if (source.programs != kGraphInvalidIndex)
                {
                    const auto* binding = ops(source.programs);
                    if (!binding || !binding->rasterPrograms)
                        return cxx::unexpected(RenderError{kStorageContract, {source.programs}});
                }
                if (source.draw_list != kGraphInvalidIndex)
                {
                    const auto* binding = ops(source.draw_list);
                    if (!binding || !binding->readDrawList)
                        return cxx::unexpected(RenderError{kStorageContract, {source.draw_list}});
                }
            }
            for (const auto& extent : manifest.buffer_extents)
            {
                const auto* binding = ops(extent.requirement);
                if (!binding || !binding->bindBufferExtent)
                    return cxx::unexpected(RenderError{kStorageContract, {extent.requirement, extent.field}});
            }
            manifest.operands.reserve(declarations.operands.size());
            for (auto& operand : declarations.operands)
            {
                const bool scoped = operand.requirement != kGraphInvalidIndex;
                const auto type = scoped ? manifest.requirements[operand.requirement].type :
                    registry.typeId(ResourceNameView{operand.type.name()});
                const auto* factory = registry.factory(type);
                const auto error = factory ? operandFactoryError(operand, *factory, features) : kStorageMismatch;
                if (scoped && error) return cxx::unexpected(RenderError{error, {manifest.operands.size()}});
                auto& linked = manifest.operands.emplace_back(
                    std::move(operand.binding), type, operand.projection, operand.requirement, operand.optional, error
                );
                if (auto* gpu = std::get_if<ResourceProjection>(&operand.contract))
                {
                    linked.gpu_import = static_cast<std::uint32_t>(manifest.gpu_imports.size());
                    manifest.gpu_imports.emplace_back(std::move(*gpu), operand.graph_uses, std::move(operand.slot));
                }
            }
            return {};
        }

        RenderResult<std::vector<std::uint32_t>> linkResourceRequirements(
            EffectDeclarations& declarations,
            std::span<const GraphPassId> order,
            std::span<const CompiledRasterStream> streams,
            const FrameAlgorithms& algorithms
        ) noexcept
        {
            for (std::uint32_t i = 0; i < streams.size(); ++i)
            {
                const auto& stream = streams[i];
                const auto pass = stream.pass;
                const auto source = order[pass];
                if (!stream.resource_list)
                {
                    const auto& outputs = algorithms.output_by_pass;
                    if (outputs.empty() || outputs[pass] == kGraphInvalidIndex)
                        return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {pass}});
                }
                const auto& sources = declarations.raster_sources;
                const auto pass_index = [](const auto& row) noexcept { return row.pass.index; };
                const auto found = std::ranges::lower_bound(sources, source.index, {}, pass_index);
                if (found != sources.end() && found->pass == source)
                    declarations.resources.raster_sources.push_back({i, found->programs, found->draw_list});
            }
            // Assign final requirement slots in first installation order. Culled streams and extents can
            // leave declarations with no consumer; those require neither a factory nor an instance.
            std::vector<std::uint32_t> linked(declarations.requirements.size(), kGraphInvalidIndex);
            std::vector<std::uint32_t> used;
            const auto link_requirement = [&](std::uint32_t& slot) noexcept
            {
                if (slot == kGraphInvalidIndex) return;
                auto& final = linked[slot];
                if (final == kGraphInvalidIndex)
                {
                    final = static_cast<std::uint32_t>(used.size());
                    used.push_back(slot);
                }
                slot = final;
            };
            auto& resources = declarations.resources;
            for (auto& source : resources.raster_sources) link_requirement(source.programs);
            for (auto& extent : resources.buffer_extents) link_requirement(extent.requirement);
            // Runtime links dynamic product reads once per consuming batch, across all of its lists.
            std::ranges::sort(resources.raster_sources, {}, [&](const auto& source) noexcept
            {
                return std::pair{streams[source.stream].batch, source.stream};
            });
            for (auto& source : resources.raster_sources) link_requirement(source.draw_list);
            for (auto& operand : declarations.operands) link_requirement(operand.requirement);
            return used;
        }
    } // namespace

    exec::task<RenderGraphCompiler::EffectResult>
    RenderGraphCompiler::compile(const effect_detail::ValidatedEffectGraph& effect, EffectCompileConfig config) & noexcept
    {
        // Evaluated on the control owner after child work rejoins.
        const auto interrupted = [&]() noexcept -> RenderError
        {
            if (environment_.stopping.load()) return {kCommandCancelled};
            return environment_.device_failure.type ? environment_.device_failure : session_.terminalError();
        };
        const auto& features = session_.enabledFeatures();
        const auto& limits = session_.description();
        const auto& dispatch = limits.shaderStage.compute.dispatchMaxDim;
        const auto available = session_.resourceFeatures();
        const EffectBuildDevice device{
            features.shader_target, session_.usesDeviceAddresses(), features.indirect_first_instance,
            {dispatch[0], dispatch[1], dispatch[2]}, limits.memory.storageBufferMaxRange,
            limits.dimensions.textureLayerMaxNum, available.containsAny(EGpuResourceFeature::SAMPLED_IMAGE_ARRAY),
            available.containsAny(EGpuResourceFeature::STORAGE_BUFFER_ARRAY)
        };
        const auto owner = co_await stdexec::read_env(stdexec::get_scheduler);
        EffectDeclarations declarations;
        GraphDefinition definition;
        auto analysis = co_await (
            stdexec::schedule(environment_.cpu) |
            stdexec::then([&]() noexcept -> RenderResult<RenderGraphAnalysis>
            {
                auto lowered = lowerEffect(effect, config, device, declarations);
                if (!lowered) return cxx::unexpected(lowered.error());
                definition = std::move(*lowered);
                return analyzeEffectResources(definition, declarations, device.shader_target);
            }) |
            stdexec::upon_stopped([]() noexcept -> RenderResult<RenderGraphAnalysis>
            {
                return cxx::unexpected(RenderError{kCpuStopped});
            }) |
            stdexec::continues_on(owner) |
            stdexec::upon_error(detail::NativeDeliveryFailure<RenderGraphAnalysis>{})
        );
        if (!analysis) co_return cxx::unexpected(analysis.error());
        if (const auto failure = interrupted(); failure.type) co_return cxx::unexpected(failure);

        std::vector<GraphResourceId> concurrent_reads;
        for (std::size_t i = 0; i < declarations.operands.size(); ++i)
        {
            const auto resource = declarations.operands[i].slot.resource;
            if (resource.index == kGraphInvalidIndex) continue;
            const auto* storage = std::get_if<ResourceProjection>(&declarations.operands[i].contract);
            if (storage && storage->concurrent_image_read) concurrent_reads.push_back(resource);
        }
        const AnalyzedGraph analyzed{definition, *analysis, concurrent_reads};
        auto plan = co_await compileNative(session_, environment_.cpu, analyzed, {.target = config.output});
        if (!plan) co_return cxx::unexpected(plan.error());
        cxx::scope_exit rollback([&]() noexcept { plan->programs_.discardUnsubmitted(); });
        if (const auto failure = interrupted(); failure.type) co_return cxx::unexpected(failure);

        std::vector<compiled::BindingComparison> comparisons;
        const auto resolve_operand = [&](GraphResourceId id) noexcept -> RenderResult<compiled::BindingOperand>
        {
            using Domain = compiled::BindingOperand::EDomain;
            const auto targets = std::span{declarations.target_imports};
            const auto target = std::ranges::find(targets, id, &TargetImageSlot::resource);
            if (target != targets.end())
                return compiled::BindingOperand{Domain::TARGET, static_cast<std::uint32_t>(target - targets.begin())};
            const auto storages = std::span{declarations.operands};
            const auto storage = std::ranges::find(storages, id, [](const auto& input) { return input.slot.resource; });
            if (storage != storages.end())
                return compiled::BindingOperand{Domain::STORAGE, static_cast<std::uint32_t>(storage - storages.begin())};
            return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {id.index}});
        };
        comparisons.reserve(definition.bindingPredicates().size());
        for (const auto& predicate : definition.bindingPredicates())
        {
            auto left = resolve_operand(predicate.left);
            if (!left) co_return cxx::unexpected(left.error());
            auto right = resolve_operand(predicate.right);
            if (!right) co_return cxx::unexpected(right.error());
            comparisons.push_back({*left, *right});
        }
        for (auto& input : declarations.target_imports)
        {
            auto& slot = input.resource;
            const auto& resource = plan->resources()[slot.index];
            if (!resource.memory_used) slot = {};
            else if (!resource.sampled_target_input)
                co_return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {slot.index}});
            if (slot.index == kGraphInvalidIndex) continue;
            const auto batches = plan->submissions();
            for (std::uint32_t b = 0; b < batches.size(); ++b)
            {
                const auto& batch = batches[b];
                const auto read = std::ranges::lower_bound(
                    batch.resource_frontiers, slot.index, {}, &ResourceUseStages::resource_index
                );
                if (read == batch.resource_frontiers.end() || read->resource_index != slot.index) continue;
                if (read->stages != nri::StageBits::NONE)
                    input.first_reads[session_.queueIndex(session_.queue(batch.queue))] = {b, read->stages};
            }
            const bool has_read = std::ranges::any_of(input.first_reads, [](const auto& wait)
            {
                return wait.first_batch != kGraphInvalidIndex;
            });
            if (!has_read)
                co_return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {slot.index}});
        }
        for (auto& declaration : declarations.operands)
        {
            auto& slot = declaration.slot;
            const auto id = slot.resource.index;
            if (id != kGraphInvalidIndex && !plan->resources()[id].memory_used) slot = {};
        }
        EffectDeviceGraph result;
        if (!declarations.inline_copies.empty() || !declarations.algorithms.empty())
        {
            FrameAlgorithmLink linker{declarations, analyzed, *plan, result};
            auto linked = linker.link(features.shader_target, plan->dynamic_buffers_);
            if (!linked) co_return cxx::unexpected(linked.error());
        }
        plan->prepared_buffer_count_ = plan->dynamic_buffers_.size();
        for (const auto& buffer : declarations.buffers)
        {
            if (buffer.size_requirement == kGraphInvalidIndex || !plan->resources()[buffer.resource.index].memory_used)
                continue;
            plan->dynamic_buffers_.push_back({buffer.resource.index, buffer.minimum_bytes, buffer.maximum_bytes, false, true});
            declarations.resources.buffer_extents.push_back({buffer.size_requirement, buffer.size_field});
        }
        for (const auto& block : declarations.uniforms)
        {
            if (!plan->resources()[block.resource.index].memory_used) continue;
            if (!config.uploads_available) co_return cxx::unexpected(RenderError{kDeviceUnsupported});
            const auto state = effectState(*block.node, block.source.state_type);
            if (!state) co_return cxx::unexpected(state.error());
            const auto value = block.source.bind(*state);
            if (value.bytes.size() != block.source.block.bytes || !value.revision)
                co_return cxx::unexpected(RenderError{kFrameContract, {block.resource.index}});
            result.constant_buffers.push_back({block.resource, nullptr, value});
        }
        if (const auto failure = interrupted(); failure.type) co_return cxx::unexpected(failure);
        auto outputs = linkBufferOutputs(declarations, analyzed, *plan, session_);
        if (!outputs) co_return cxx::unexpected(outputs.error());
        auto& graph = *plan;
        auto linked = linkResourceRequirements(
            declarations, analysis->execution_order, graph.programs().rasterStreams(), result.algorithms
        );
        if (!linked) co_return cxx::unexpected(linked.error());
        if (auto manifest = linkResourceManifest(declarations, *linked, config.resource_types, available); !manifest)
            co_return cxx::unexpected(manifest.error());
        graph.target_imports_ = std::move(declarations.target_imports);
        result.resources = std::move(declarations.resources);
        graph.binding_comparisons_ = std::move(comparisons);
        rollback.release();
        result.graph = std::make_unique<const compiled::RenderGraph>(std::move(graph));
        co_return result;
    }
} // namespace lux::render::detail
