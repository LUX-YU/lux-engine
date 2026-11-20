#include <lux/engine/render/effect/EffectPreparation.hpp>
#include <lux/engine/render/runtime/RenderEffect.hpp>
#include <lux/engine/render/graph/RenderGraphExecutable.hpp>
#include <lux/engine/render/graph/compile/GraphLiveness.hpp>
#include <lux/engine/render/graph/execute/RasterStreamPlan.hpp>

namespace lux::render::runtime_detail
{
    namespace
    {
        class LinkContext final : public InstanceLinkContext
        {
        public:
            LinkContext(
                const PreparationDefinition& definition, const GraphPreparation& producer,
                std::span<const std::uint32_t> data_slots, std::span<const std::uint32_t> buffer_slots,
                std::span<const std::uint32_t> snapshot_slots, const RenderGraph& source,
                const detail::GraphAssemblyPlan& assembly, const detail::RenderGraphExecutable& executable,
                const detail::NativeGraphPrograms& programs, std::vector<detail::LinkedInlineBlock>& constants,
                std::vector<std::uint32_t>& buffers,
                std::vector<std::uint32_t>& streams, std::vector<std::uint8_t>& claimed_streams,
                const RenderEffect::NodeState* state, std::uint32_t producer_slot
            ) noexcept : definition_(definition), producer_(producer), data_slots_(data_slots),
                         buffer_slots_(buffer_slots), snapshot_slots_(snapshot_slots), source_(source),
                         assembly_(assembly), executable_(executable),
                         programs_(programs), constants_(constants), buffers_(buffers), streams_(streams),
                         claimed_streams_(claimed_streams),
                         state_(state), producer_slot_(producer_slot)
            {
            }

            std::span<const std::byte> node_state;

        private:
            RenderResult<std::uint32_t>
            resolveStorageSlot(GraphStorageSnapshotId id, cxx::basic_type_info type) const noexcept override
            {
                const bool declared = std::ranges::find(producer_.storage_inputs, id) != producer_.storage_inputs.end();
                if (!declared)
                    return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, id.index}});
                const auto expected = definition_.snapshots[id.index].type;
                if (expected != type || expected.name() != type.name())
                    return cxx::unexpected(RenderError{kStorageMismatch, {id.index}});
                return snapshot_slots_[id.index];
            }

            RenderResult<std::uint32_t> claimNodeState(cxx::basic_type_info type, std::size_t bytes) noexcept override
            {
                if (!state_ || state_->type != type || state_->bytes != bytes)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {producer_slot_}});
                node_state = {static_cast<const std::byte*>(state_->value.get()), bytes};
                return producer_slot_;
            }

            RenderResult<DrawStreamSlot> resolveDrawStream(GraphPassId pass) noexcept override
            {
                auto resolved = streamPass(pass);
                if (!resolved) return cxx::unexpected(resolved.error());
                if (claimed_streams_[*resolved])
                    return cxx::unexpected(RenderError{kGraphAmbiguousProducer, {pass.index}});
                claimed_streams_[*resolved] = 1;
                streams_.push_back(*resolved);
                return DrawStreamSlot{*resolved};
            }

            RenderResult<RasterProgramSlot>
            resolveRasterProgram(GraphPassId pass, std::uint32_t source_program) const noexcept override
            {
                auto resolved = streamPass(pass);
                if (!resolved) return cxx::unexpected(resolved.error());
                const auto& raster = std::get<RasterWork>(source_.passes()[pass.index].command);
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
                const auto& buffers = programs_.rasterStream(*resolved)->buffers;
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
                const auto& images = programs_.rasterStream(*resolved)->images;
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
                const auto& raster = std::get<RasterWork>(source_.passes()[pass.index].command);
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
                const auto& raster = std::get<RasterWork>(source_.passes()[pass.index].command);
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
                const auto slot = assembly_.source_pass_slots[pass.index];
                if (slot >= executable_.passes().size() || !programs_.rasterStream(slot))
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {pass.index}});
                return slot;
            }

            RenderResult<std::uint32_t>
            resolveCpuSlot(GraphCpuDataId id, cxx::basic_type_info type, std::size_t bytes) const noexcept override
            {
                const bool declared = std::ranges::find(producer_.inputs, id) != producer_.inputs.end() ||
                    std::ranges::find(producer_.outputs, id) != producer_.outputs.end();
                if (!declared) return cxx::unexpected(RenderError{kGraphUnknownResource, {id.index}});
                const auto& data = definition_.cpu_data[id.index];
                if (data.type != type || data.bytes != bytes)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {kGraphInvalidIndex, id.index}});
                return data_slots_[id.index];
            }

            RenderResult<std::uint32_t>
            claimInlineBlock(GraphPassId id, cxx::basic_type_info type, std::size_t bytes) noexcept override
            {
                if (std::ranges::find(producer_.inline_blocks, id) == producer_.inline_blocks.end())
                    return cxx::unexpected(RenderError{kGraphUnknownPass, {id.index}});
                const auto slot = assembly_.source_pass_slots[id.index];
                const auto& contract = source_.passes()[id.index].interface.parameters.inline_constant;
                const bool valid_slot = slot < executable_.passes().size();
                const bool compatible = valid_slot && executable_.passes()[slot].parameter_size == bytes &&
                    contract.type == type;
                if (!compatible) return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {id.index}});
                const auto previous = std::ranges::find(constants_, slot, &detail::LinkedInlineBlock::pass);
                if (previous != constants_.end())
                    return cxx::unexpected(RenderError{kGraphAmbiguousProducer, {id.index}});
                constants_.push_back({slot, bytes});
                return slot;
            }

            RenderResult<std::uint32_t> claimPreparedBuffer(GraphResourceId resource, bool upload) noexcept override
            {
                const auto declared = std::ranges::find(producer_.gpu_buffers, resource);
                if (declared == producer_.gpu_buffers.end())
                    return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, resource.index}});
                const auto& buffer = source_.resources()[resource.index];
                if (upload != (buffer.origin == EGraphOrigin::IMPORTED))
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {kGraphInvalidIndex, resource.index}});
                const auto slot = buffer_slots_[resource.index];
                if (slot == kGraphInvalidIndex) std::terminate();
                if (std::ranges::find(buffers_, slot) != buffers_.end())
                    return cxx::unexpected(RenderError{kGraphAmbiguousProducer, {kGraphInvalidIndex, resource.index}});
                buffers_.push_back(slot);
                return slot;
            }

            const PreparationDefinition& definition_;
            const GraphPreparation& producer_;
            std::span<const std::uint32_t> data_slots_;
            std::span<const std::uint32_t> buffer_slots_;
            std::span<const std::uint32_t> snapshot_slots_;
            const RenderGraph& source_;
            const detail::GraphAssemblyPlan& assembly_;
            const detail::RenderGraphExecutable& executable_;
            const detail::NativeGraphPrograms& programs_;
            std::vector<detail::LinkedInlineBlock>& constants_;
            std::vector<std::uint32_t>& buffers_;
            std::vector<std::uint32_t>& streams_;
            std::vector<std::uint8_t>& claimed_streams_;
            const RenderEffect::NodeState* state_;
            std::uint32_t producer_slot_;
        };
    }

    RenderResult<RenderGraphAnalysis>
    analyzePreparedGraph(const RenderGraph& source, PreparationDefinition& definition) noexcept
    {
        for (const auto& pass : source.passes())
        {
            const auto* raster = std::get_if<RasterWork>(&pass.command);
            const auto* stage = raster ? std::get_if<RasterPassTemplate>(&raster->program) : nullptr;
            if (!stage) continue;
            const bool invalid_textures = stage->shared_textures &&
                stage->shared_textures->index >= definition.texture_sets.size();
            const bool invalid_meshes = stage->mesh_geometry && stage->mesh_geometry->index >= definition.mesh_sets.size();
            const bool invalid_materials = stage->shared_materials &&
                stage->shared_materials->index >= definition.material_sets.size();
            if (invalid_materials)
                return cxx::unexpected(RenderError{kGraphUnknownResource,
                    {kGraphInvalidIndex, stage->shared_materials->index}});
            if (invalid_textures || invalid_meshes)
            {
                const auto index = invalid_textures ? stage->shared_textures->index : stage->mesh_geometry->index;
                return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, index}});
            }
        }
        const auto count = static_cast<std::uint32_t>(definition.producers.size());
        if (!count && !definition.buffers.empty())
            return cxx::unexpected(RenderError{kGraphUndefinedRead, {kGraphInvalidIndex}});
        if (!count) return analyzeRenderGraph(source);
        detail::PreparationLiveness liveness;
        liveness.producers.resize(count);
        liveness.pass_producers.resize(source.passes().size());
        std::vector<std::uint32_t> writers(definition.cpu_data.size(), kGraphInvalidIndex);
        std::vector<std::uint32_t> buffer_writers(source.resources().size(), kGraphInvalidIndex);
        std::vector<std::uint32_t> pass_writers(source.passes().size(), kGraphInvalidIndex);
        std::vector<std::vector<std::uint32_t>> successors(count);
        std::vector<std::uint32_t> indegree(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const auto& declaration = definition.producers[i].declaration;
            auto& producer = liveness.producers[i];
            producer.passes = declaration.passes;
            for (auto input : declaration.storage_inputs)
            {
                if (input.index >= definition.snapshots.size())
                    return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, input.index}});
            }
            for (auto input : declaration.program_storage_inputs)
            {
                if (std::ranges::find(declaration.storage_inputs, input) == declaration.storage_inputs.end())
                    return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, input.index}});
                if (std::ranges::count(declaration.program_storage_inputs, input) != 1)
                    return cxx::unexpected(RenderError{kGraphAmbiguousProducer, {kGraphInvalidIndex, input.index}});
            }
            for (auto output : declaration.outputs)
            {
                if (output.index >= writers.size())
                    return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, output.index}});
                auto& writer = writers[output.index];
                if (writer != kGraphInvalidIndex)
                    return cxx::unexpected(RenderError{kGraphAmbiguousProducer, {kGraphInvalidIndex, output.index}});
                writer = i;
            }
            for (auto output : declaration.gpu_buffers)
            {
                const auto buffer = std::ranges::find(definition.buffers, output, &PreparationDefinition::Buffer::resource);
                if (buffer == definition.buffers.end() || buffer->node != definition.producers[i].node)
                    return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, output.index}});
                auto& writer = buffer_writers[output.index];
                if (writer != kGraphInvalidIndex)
                    return cxx::unexpected(RenderError{kGraphAmbiguousProducer, {kGraphInvalidIndex, output.index}});
                writer = i;
            }
            for (auto pass : declaration.inline_blocks)
            {
                if (pass.index >= pass_writers.size())
                    return cxx::unexpected(RenderError{kGraphUnknownPass, {pass.index}});
                if (!source.passes()[pass.index].interface.parameters.inline_constant.layout)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {pass.index}});
                auto& writer = pass_writers[pass.index];
                if (writer != kGraphInvalidIndex)
                    return cxx::unexpected(RenderError{kGraphAmbiguousProducer, {pass.index}});
                writer = i;
                if (std::ranges::find(producer.passes, pass) == producer.passes.end())
                    producer.passes.push_back(pass);
            }
            if (producer.passes.empty() && declaration.outputs.empty() && declaration.gpu_buffers.empty())
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {i}});
            for (auto pass : producer.passes)
            {
                if (pass.index >= liveness.pass_producers.size())
                    return cxx::unexpected(RenderError{kGraphUnknownPass, {pass.index}});
                liveness.pass_producers[pass.index].push_back(i);
            }
        }
        for (const auto& buffer : definition.buffers)
        {
            if (buffer_writers[buffer.resource.index] == kGraphInvalidIndex)
                return cxx::unexpected(RenderError{kGraphUndefinedRead, {kGraphInvalidIndex, buffer.resource.index}});
        }
        for (std::uint32_t pass = 0; pass < source.passes().size(); ++pass)
        {
            auto& producers = liveness.pass_producers[pass];
            for (const auto& access : source.passes()[pass].uses)
            {
                const bool frame_buffer_fallback = access.fallback &&
                    source.resources()[access.fallback->resource.index].origin == EGraphOrigin::FRAME_BUFFER;
                if (frame_buffer_fallback)
                    return cxx::unexpected(RenderError{
                        kGraphInvalidDescriptor, {pass, access.fallback->resource.index}
                    });
                const auto resource = access.version.resource.index;
                const auto writer = buffer_writers[resource];
                if (writer == kGraphInvalidIndex) continue;
                const bool gpu_written = source.resources()[resource].origin == EGraphOrigin::FRAME_BUFFER;
                const bool invalid_access = gpu_written ? !access.whole_resource : access.access != EGraphAccess::READ;
                if (invalid_access || access.fallback)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {pass, resource}});
                if (std::ranges::find(producers, writer) == producers.end()) producers.push_back(writer);
            }
        }
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const auto& inputs = definition.producers[i].declaration.inputs;
            for (auto input : inputs)
            {
                if (input.index >= writers.size())
                    return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, input.index}});
                const auto writer = writers[input.index];
                if (writer == kGraphInvalidIndex)
                    return cxx::unexpected(RenderError{kGraphUndefinedRead, {kGraphInvalidIndex, input.index}});
                liveness.producers[i].inputs.push_back(writer);
                successors[writer].push_back(i);
                ++indegree[i];
            }
        }
        std::vector<std::uint32_t> order;
        order.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            if (indegree[i] == 0) order.push_back(i);
        }
        for (std::size_t i = 0; i < order.size(); ++i)
        {
            for (auto successor : successors[order[i]])
            {
                if (--indegree[successor] == 0) order.push_back(successor);
            }
        }
        if (order.size() != count) return cxx::unexpected(RenderError{kGraphDependencyCycle});
        auto analysis = detail::analyzeGraphWithPreparation(source, &liveness);
        if (!analysis) return cxx::unexpected(analysis.error());
        for (auto producer : order)
        {
            if (liveness.producers[producer].live) definition.live_order.push_back(producer);
        }
        return analysis;
    }

    RenderResult<detail::PreparationPlan> linkPreparation(
        const PreparationDefinition& definition,
        const RenderGraph& source,
        const detail::GraphAssemblyPlan& assembly,
        const detail::RenderGraphExecutable& executable,
        const detail::NativeGraphPrograms& native,
        const RenderEffect& effect
    ) noexcept
    {
        detail::PreparationPlan plan;
        for (std::size_t p = 0; p < source.passes().size(); ++p)
        {
            const auto slot = assembly.source_pass_slots[p];
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
        std::vector<std::uint8_t> claimed_streams(executable.passes().size());
        std::vector<std::uint32_t> data_slots(definition.cpu_data.size(), kGraphInvalidIndex);
        std::vector<std::uint32_t> buffer_slots(source.resources().size(), kGraphInvalidIndex);
        std::vector<std::uint32_t> snapshot_slots(definition.snapshots.size(), kGraphInvalidIndex);
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
                    executable.resources()[output.index].memory_used
                });
            }
        }

        plan.producers.reserve(definition.live_order.size());
        for (auto index : definition.live_order)
        {
            const auto& [declaration, node] = definition.producers[index];
            const auto state = std::ranges::find(effect.node_states, node, &RenderEffect::NodeState::node);
            std::vector<detail::LinkedInlineBlock> constants;
            std::vector<std::uint32_t> buffers;
            std::vector<std::uint32_t> streams;
            LinkContext context{
                definition, declaration, data_slots, buffer_slots, snapshot_slots, source, assembly, executable, native,
                constants, buffers, streams, claimed_streams,
                state == effect.node_states.end() ? nullptr : &*state,
                static_cast<std::uint32_t>(plan.producers.size())
            };
            auto prototype = declaration.factory.link(declaration.configuration, context);
            if (!prototype) return cxx::unexpected(prototype.error());
            const auto expected_writers = std::ranges::count_if(declaration.inline_blocks, [&](GraphPassId pass)
            {
                return assembly.source_pass_slots[pass.index] != kGraphInvalidIndex;
            });
            if (constants.size() != static_cast<std::size_t>(expected_writers) ||
                buffers.size() != declaration.gpu_buffers.size())
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {index}});

            auto& linked = plan.producers.emplace_back(
                std::move(*prototype),
                std::vector<std::uint32_t>{},
                std::vector<std::uint32_t>{},
                std::move(constants),
                std::move(streams),
                std::move(buffers),
                std::vector<std::uint32_t>{},
                context.node_state,
                std::vector<std::uint32_t>{}
            );
            for (auto input : declaration.inputs) linked.inputs.push_back(data_slots[input.index]);
            for (auto input : declaration.storage_inputs) linked.snapshots.push_back(snapshot_slots[input.index]);
            for (auto input : declaration.program_storage_inputs)
                linked.program_snapshots.push_back(snapshot_slots[input.index]);
            for (auto output : declaration.outputs) linked.outputs.push_back(data_slots[output.index]);
        }
        for (std::uint32_t p = 0; p < claimed_streams.size(); ++p)
        {
            if (native.rasterStream(p) && !claimed_streams[p])
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {p}});
        }
        return plan;
    }
} // namespace lux::render::runtime_detail
