#include <lux/engine/render/effect/EffectPreparation.hpp>
#include <utility>
#include <lux/engine/render/runtime/RenderEffect.hpp>
#include <lux/engine/render/graph/RenderGraphExecutable.hpp>
#include <lux/engine/render/graph/compile/GraphLiveness.hpp>
#include <lux/engine/render/graph/execute/RasterStreamPlan.hpp>

namespace lux::render::runtime_detail
{
    namespace
    {
        struct PreparationLinkWorkspace final
        {
            const EffectGraphBuild& build;
            const detail::PreparedRenderGraph& compiled;
            std::vector<std::uint32_t> data_slots, buffer_slots, snapshot_slots;
            std::vector<std::uint8_t> claimed_streams;
            detail::PreparationPlan plan;

            PreparationLinkWorkspace(const EffectGraphBuild& graph, const detail::PreparedRenderGraph& native) noexcept
                : build(graph), compiled(native), data_slots(build.preparation.cpu_data.size(), kGraphInvalidIndex),
                  buffer_slots(build.source.resources().size(), kGraphInvalidIndex),
                  snapshot_slots(build.preparation.snapshots.size(), kGraphInvalidIndex),
                  claimed_streams(compiled.executable.passes().size())
            {
                assignSlots();
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
                const RenderEffect::NodeState* state,
                std::uint32_t slot
            ) noexcept : workspace_(workspace), producer_(producer), state_(state), producer_slot_(slot)
            {
            }

            RenderResult<detail::LinkedPreparationProducer> link(std::uint32_t source_index) noexcept
            {
                auto prototype = producer_.factory.link(producer_.configuration, *this);
                if (!prototype) return cxx::unexpected(prototype.error());
                const auto expected = std::ranges::count_if(producer_.inline_blocks, [&](GraphPassId pass) noexcept
                {
                    return workspace_.compiled.assembly.source_pass_slots[pass.index].valid();
                });
                const bool missing_constants = constants_.size() != static_cast<std::size_t>(expected);
                const bool missing_buffers = buffers_.size() != producer_.gpu_buffers.size();
                if (missing_constants || missing_buffers)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {source_index}});

                detail::LinkedPreparationProducer linked{
                    .prototype = std::move(*prototype),
                    .inline_blocks = std::move(constants_),
                    .raster_streams = std::move(streams_),
                    .buffers = std::move(buffers_),
                    .node_state = node_state_
                };
                for (auto id : producer_.inputs) linked.inputs.push_back(workspace_.data_slots[id.index]);
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

            RenderResult<std::uint32_t> claimNodeState(cxx::basic_type_info type, std::size_t bytes) noexcept override
            {
                if (!state_ || state_->type != type || state_->bytes != bytes)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {producer_slot_}});
                node_state_ = {static_cast<const std::byte*>(state_->value.get()), bytes};
                return producer_slot_;
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
                const auto slot = workspace_.compiled.assembly.source_pass_slots[pass.index].index;
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
                const auto slot = workspace_.compiled.assembly.source_pass_slots[id.index].index;
                const auto& contract = workspace_.build.source.passes()[id.index].interface.parameters.inline_constant;
                const bool valid_slot = slot < workspace_.compiled.executable.passes().size();
                const bool compatible = valid_slot &&
                    workspace_.compiled.executable.passes()[slot].parameter_size == bytes &&
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
            std::span<const std::byte> node_state_;
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

    RenderResult<detail::PreparationPlan>
    linkPreparation(
        const EffectGraphBuild& build, const detail::PreparedRenderGraph& compiled, const RenderEffect& effect
    ) noexcept
    {
        const auto& definition = build.preparation;
        const auto& source = build.source;
        const auto& assembly = compiled.assembly;
        const auto& executable = compiled.executable;
        PreparationLinkWorkspace workspace(build, compiled);
        auto& plan = workspace.plan;
        for (std::size_t p = 0; p < source.passes().size(); ++p)
        {
            const auto slot = assembly.source_pass_slots[p].index;
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
        plan.producers.reserve(definition.live_order.size());
        for (auto index : definition.live_order)
        {
            const auto& [declaration, node] = definition.producers[index];
            const auto state = std::ranges::find(effect.node_states, node, &RenderEffect::NodeState::node);
            LinkContext context{
                workspace, declaration, state == effect.node_states.end() ? nullptr : &*state,
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
} // namespace lux::render::runtime_detail
