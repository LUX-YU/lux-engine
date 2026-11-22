#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/extensions/execution/FramePreparation.hpp>
#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/device/RasterProgramUpdate.hpp>
#include <lux/engine/render/memory/Cleanup.hpp>
#include <lux/engine/render/resource/material/RasterMaterialStorage.hpp>
#include <lux/engine/render/resource/mesh/IndexedTriangleMeshStorage.hpp>
#include <lux/engine/render/resource/texture/SampledTexture2DStorage.hpp>
#include <algorithm>
#include <bit>

namespace lux::render::detail
{
    namespace
    {
        OwnedRasterStream::TextureRead textureRead(const SampledTexture2DStorage::SampledImage& image) noexcept
        {
            return {&image.image(), image.description().mipNum, image.description().layerNum};
        }

    }

    class RenderGraphInstance::ProducerFrame final : public FramePreparation
    {
    public:
        ProducerFrame(RenderGraphInstance& instance, std::uint32_t producer_slot, std::uint32_t frame_slot) noexcept
            : instance_(instance), linked_(instance.preparation_plan_->producers[producer_slot]),
              producer_(instance.producers_[producer_slot]), producer_slot_(producer_slot), frame_slot_(frame_slot)
        {
        }

        const TargetDesc& target() const noexcept override { return instance_.target(); }

        RenderResult<void> setRasterStream(DrawStreamSlot slot, const RasterStreamData& data) noexcept override
        {
            const auto use = std::ranges::find(linked_.raster_streams, slot.index);
            if (use == linked_.raster_streams.end())
                return cxx::unexpected(RenderError{kGraphUnknownPass, {slot.index}});
            auto& stream = producer_.streams[use - linked_.raster_streams.begin()];
            if (stream.written) return cxx::unexpected(RenderError{kFrameContract, {slot.index}});
            const bool outside_limits = data.draws.size() >= kGraphInvalidIndex ||
                data.vertices.size() >= kGraphInvalidIndex || data.bindings.size() >= kGraphInvalidIndex ||
                data.constants.size() >= kGraphInvalidIndex || data.sections.size() >= kGraphInvalidIndex;
            if (outside_limits) return cxx::unexpected(RenderError{kDeviceCapacity});
            stream.draws.assign(data.draws.begin(), data.draws.end());
            stream.bindings.assign(data.bindings.begin(), data.bindings.end());
            stream.vertices.assign(data.vertices.begin(), data.vertices.end());
            stream.constants.assign(data.constants.begin(), data.constants.end());
            stream.sections.assign(data.sections.begin(), data.sections.end());
            stream.written = true;
            return {};
        }

        RenderResult<RasterProgramSlot>
        rasterProgram(DrawStreamSlot slot, std::uint32_t selection_index) const noexcept override
        {
            const auto& streams = producer_.runtime_streams;
            if (std::ranges::find(streams, slot.index) == streams.end())
                return cxx::unexpected(RenderError{kGraphUnknownPass, {slot.index}});
            const auto& selected = instance_.dynamic_raster_programs_[slot.index];
            if (!selected || selection_index >= selected->programs.size())
                return cxx::unexpected(RenderError{kFrameContract, {slot.index, selection_index}});
            return RasterProgramSlot{slot.index, selection_index, selected->revision};
        }

        RenderResult<TextureGpuRef> sampleTexture(DrawStreamSlot slot, TextureId id) noexcept override
        {
            const auto use = std::ranges::find(linked_.raster_streams, slot.index);
            const auto& owners = instance_.raster_owners_;
            const bool missing_owner = slot.index >= owners.size() || !owners[slot.index].textures;
            if (use == linked_.raster_streams.end() || missing_owner)
                return cxx::unexpected(RenderError{kGraphUnknownPass, {slot.index}});
            const auto* record = owners[slot.index].textures->find(id);
            if (!record) return cxx::unexpected(RenderError{kStorageIdentity, {slot.index}});
            producer_.streams[use - linked_.raster_streams.begin()].textures.push_back(textureRead(*record));
            return record->gpuReference();
        }

        RenderResult<RasterMeshGeometry> selectMesh(DrawStreamSlot slot, MeshId id) noexcept override
        {
            const auto use = std::ranges::find(linked_.raster_streams, slot.index);
            const auto& owners = instance_.raster_owners_;
            const bool missing_owner = slot.index >= owners.size() || !owners[slot.index].meshes;
            if (use == linked_.raster_streams.end() || missing_owner)
                return cxx::unexpected(RenderError{kGraphUnknownPass, {slot.index}});
            auto views = owners[slot.index].meshes->bufferViews(id);
            if (!views) return cxx::unexpected(views.error());

            auto& stream = producer_.streams[use - linked_.raster_streams.begin()];
            if (stream.mesh_buffers.size() > kGraphInvalidIndex - 2)
                return cxx::unexpected(RenderError{kDeviceCapacity, {slot.index}});
            const auto first = static_cast<std::uint32_t>(stream.mesh_buffers.size());
            const auto& vertices = views->vertices;
            const auto& indices = views->indices;
            const auto first_index = indices.offset / views->index_stride;
            const auto vertex_offset = vertices.offset / views->vertex_stride;
            if (first_index > UINT32_MAX || vertex_offset > INT32_MAX)
                return cxx::unexpected(RenderError{kStorageCapacity, {slot.index}});
            stream.mesh_buffers.push_back({vertices.buffer, vertices.offset, vertices.size, views->layout, EGraphUse::VERTEX});
            stream.mesh_buffers.push_back({indices.buffer, indices.offset, indices.size, views->layout, EGraphUse::INDEX});
            const auto type = views->index_stride == 2 ? EIndexType::UINT16 : EIndexType::UINT32;
            return RasterMeshGeometry{
                {{first, slot.index, stream.selection}, 0, vertices.size},
                {{first + 1, slot.index, stream.selection}, 0, indices.size},
                type,
                static_cast<std::uint32_t>(first_index),
                static_cast<std::int32_t>(vertex_offset)
            };
        }

        RenderResult<rdesc::MaterialGpuReference>
        selectMaterial(DrawStreamSlot slot, RasterProgramSlot program, MaterialId id) noexcept override
        {
            const auto use = std::ranges::find(linked_.raster_streams, slot.index);
            const auto& owners = instance_.raster_owners_;
            const bool missing_owner = slot.index >= owners.size() || !owners[slot.index].materials;
            if (use == linked_.raster_streams.end() || missing_owner)
                return cxx::unexpected(RenderError{kGraphUnknownPass, {slot.index}});
            const auto& compiled = *instance_.programs_->rasterStream(slot.index);
            const auto* dynamic = compiled.runtime ? instance_.dynamic_raster_programs_[slot.index].get() : nullptr;
            const auto& programs = dynamic ? dynamic->programs : compiled.programs;
            const auto revision = dynamic ? dynamic->revision : 0;
            const bool invalid_program = program.stream != slot.index || program.revision != revision ||
                program.index >= programs.size();
            if (invalid_program || !programs[program.index].record_stages)
                return cxx::unexpected(RenderError{kFrameContract, {slot.index, program.index}});

            const auto& selected = programs[program.index];
            const auto& storage = owners[slot.index];
            auto record = storage.materials->selectRasterRecord(id, frame_slot_, selected.record_layout);
            if (!record) return cxx::unexpected(record.error());
            auto& stream = producer_.streams[use - linked_.raster_streams.begin()];
            const auto first = stream.material_images.size();
            if (selected.shared_texture_stages)
            {
                const bool exceeds_capacity = record->textures.size() >= kGraphInvalidIndex - first;
                if (exceeds_capacity) return cxx::unexpected(RenderError{kDeviceCapacity, {slot.index}});
                for (auto texture : record->textures)
                {
                    if (texture.isNull()) continue;
                    const auto* image = storage.textures ? storage.textures->find(texture) : nullptr;
                    if (!image)
                    {
                        stream.material_images.resize(first);
                        return cxx::unexpected(RenderError{kStorageIdentity, {slot.index}});
                    }
                    stream.material_images.push_back(textureRead(*image));
                }
            }
            const auto& buffer = record->buffer;
            stream.materials.push_back({
                buffer.buffer, {buffer.offset, buffer.size}, program.index,
                static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(stream.material_images.size() - first)
            });
            return record->reference;
        }

    private:
        RenderResult<extension_detail::SelectedStorageSnapshot>
        readStorageValue(std::uint32_t slot, cxx::basic_type_info type) const noexcept override
        {
            return instance_.readStorageSnapshot(linked_.snapshots, slot, type);
        }

        RenderResult<SelectedBytes> readBytes(std::uint32_t slot, std::size_t bytes) const noexcept override
        {
            if (std::ranges::find(linked_.inputs, slot) == linked_.inputs.end())
                return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, slot}});
            const auto& selection = instance_.cpu_data_[slot];
            if (!selection.written)
                return cxx::unexpected(RenderError{kGraphUndefinedRead, {kGraphInvalidIndex, slot}});
            if (selection.bytes.size() != bytes)
                return cxx::unexpected(RenderError{kFrameContract, {slot}});
            return SelectedBytes{selection.bytes, instance_.preparation_revision_};
        }

        RenderResult<void> publishBytes(std::uint32_t slot, std::span<const std::byte> bytes) noexcept override
        {
            if (std::ranges::find(linked_.outputs, slot) == linked_.outputs.end())
                return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, slot}});
            return stage(instance_.cpu_data_[slot], bytes);
        }

        RenderResult<void>
        writeInlineBlock(std::uint32_t slot, std::span<const std::byte> bytes, std::uint64_t revision) noexcept override
        {
            const auto use = std::ranges::find(linked_.inline_blocks, slot, &LinkedInlineBlock::pass);
            if (use == linked_.inline_blocks.end())
                return cxx::unexpected(RenderError{kGraphUnknownPass, {slot}});
            auto& output = producer_.constants[use - linked_.inline_blocks.begin()];
            const bool unchanged = revision != 0 && revision == output.source_revision;
            auto written = stage(output.payload, bytes, unchanged);
            if (written) output.source_revision = revision;
            return written;
        }

        RenderResult<void> writeGpuBuffer(
            std::uint32_t slot, std::span<const std::byte> bytes, std::uint64_t revision
        ) noexcept override
        {
            if (!ownsBuffer(slot))
                return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, slot}});
            const auto& contract = instance_.preparation_plan_->buffers[slot];
            auto& selected = instance_.prepared_buffer_inputs_[slot];
            const bool invalid_extent =
                bytes.size() < contract.minimum_bytes || bytes.size() > contract.maximum_bytes || bytes.size() % 4;
            const bool already_written = selected.source != PreparedBufferInput::ESource::MISSING;
            if (!contract.upload || already_written || invalid_extent)
                return cxx::unexpected(RenderError{kFrameContract, {kGraphInvalidIndex, contract.resource}});
            const auto& replica = instance_.frames_[frame_slot_].native.prepared_buffers[slot];
            if (!contract.used) selected.source = PreparedBufferInput::ESource::SIZE_ONLY;
            else if (replica.contains(revision, bytes.size()))
            {
                selected.source = PreparedBufferInput::ESource::RESIDENT;
                selected.resident_generation = replica.generation;
            }
            else
            {
                selected.bytes.assign(bytes.begin(), bytes.end());
                selected.source = PreparedBufferInput::ESource::OWNED_BYTES;
            }
            selected.revision = revision;
            selected.size = bytes.size();
            return {};
        }

        RenderResult<void> sizeGpuBuffer(std::uint32_t slot, std::uint64_t size) noexcept override
        {
            if (!ownsBuffer(slot))
                return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, slot}});
            const auto& contract = instance_.preparation_plan_->buffers[slot];
            auto& selected = instance_.prepared_buffer_inputs_[slot];
            const bool invalid_extent = size < contract.minimum_bytes || size > contract.maximum_bytes || size % 4;
            const bool already_written = selected.source != PreparedBufferInput::ESource::MISSING;
            if (contract.upload || already_written || invalid_extent)
                return cxx::unexpected(RenderError{kFrameContract, {kGraphInvalidIndex, contract.resource}});
            selected.source = PreparedBufferInput::ESource::SIZE_ONLY;
            selected.size = size;
            return {};
        }

        [[nodiscard]] bool ownsBuffer(std::uint32_t slot) const noexcept
        {
            const auto& buffers = instance_.preparation_plan_->buffers;
            return slot < buffers.size() && buffers[slot].producer == producer_slot_;
        }

        static RenderResult<void>
        stage(PreparedBytes& output, std::span<const std::byte> bytes, bool unchanged = false) noexcept
        {
            if (output.written || output.bytes.size() != bytes.size())
                return cxx::unexpected(RenderError{kFrameContract});
            if (!unchanged) std::ranges::copy(bytes, output.bytes.begin());
            output.written = true;
            return {};
        }

        RenderGraphInstance& instance_;
        const LinkedPreparationProducer& linked_;
        Producer& producer_;
        std::uint32_t producer_slot_;
        std::uint32_t frame_slot_;
    };

    RenderResult<extension_detail::SelectedStorageSnapshot> RenderGraphInstance::readStorageSnapshot(
        std::span<const std::uint32_t> inputs, std::uint32_t slot, cxx::basic_type_info type
    ) const noexcept
    {
        if (std::ranges::find(inputs, slot) == inputs.end())
            return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, slot}});
        const auto& selected = storage_snapshots_[snapshot_sources_[slot]];
        const auto expected = selected.source.ops->type;
        if (expected != type || expected.name() != type.name())
            return cxx::unexpected(RenderError{kStorageMismatch, {slot}});
        return extension_detail::SelectedStorageSnapshot{selected.value.get(), selected.revision};
    }

    RenderResult<void> RenderGraphInstance::createProducers(
        const PreparationPlan& plan, const ResourceRegistry& registry, PreparationSources inputs
    ) noexcept
    {
        if (plan.producers.empty() && plan.constant_copies.empty()) return {};
        if (preparation_plan_) return cxx::unexpected(RenderError{kDeviceArgument});
        const auto& sources = inputs.snapshots;
        auto& raster_owners = inputs.raster_owners;
        if (inputs.effect_values.size() != plan.effect_inputs.size())
            return cxx::unexpected(RenderError{kFrameContract});
        for (std::size_t i = 0; i < plan.effect_inputs.size(); ++i)
        {
            const auto slot = plan.effect_inputs[i].destination.index;
            if (slot >= plan.cpu_data_bytes.size() || inputs.effect_values[i].size() != plan.cpu_data_bytes[slot])
                return cxx::unexpected(RenderError{kFrameContract, {slot}});
        }
        if (sources.size() != plan.snapshots.size()) return cxx::unexpected(RenderError{kStorageContract});
        if (raster_owners.size() != plan.raster_bindings.size())
            return cxx::unexpected(RenderError{kStorageContract});
        std::vector<StorageSnapshot> snapshots;
        snapshots.reserve(sources.size());
        std::vector<std::uint32_t> source_slots;
        source_slots.reserve(sources.size());
        for (std::size_t i = 0; i < sources.size(); ++i)
        {
            const auto& source = sources[i];
            const auto expected = plan.snapshots[i].type;
            const bool invalid_source = !source.storage || !source.ops;
            const bool wrong_type = !invalid_source &&
                (source.ops->type != expected || source.ops->type.name() != expected.name());
            if (invalid_source || wrong_type)
                return cxx::unexpected(RenderError{kStorageMismatch, {i}});
            const auto found = std::ranges::find_if(snapshots, [&](const StorageSnapshot& selected)
            {
                return selected.source.id == source.id;
            });
            source_slots.push_back(static_cast<std::uint32_t>(found - snapshots.begin()));
            if (found != snapshots.end())
            {
                if (found->source.ops != source.ops || found->source.storage != source.storage)
                    return cxx::unexpected(RenderError{kStorageMismatch, {i}});
                continue;
            }
            auto value = source.ops->create();
            if (!value) return cxx::unexpected(RenderError{kStorageContract, {i}});
            snapshots.emplace_back(source, std::move(value));
        }
        std::vector<Producer> producers;
        std::vector<std::uint32_t> program_producers;
        producers.reserve(plan.producers.size());
        for (const auto& linked : plan.producers)
        {
            auto data = linked.prototype.instantiate(target());
            if (!data) return cxx::unexpected(data.error());
            auto& producer = producers.emplace_back(
                std::move(*data), std::vector<PreparedInlineBlock>{},
                std::vector<OwnedRasterStream>(linked.raster_streams.size())
            );
            for (auto pass : linked.raster_streams)
            {
                if (programs_->rasterStream(pass)->runtime) producer.runtime_streams.push_back(pass);
            }
            if (!linked.program_snapshots.empty() && producer.runtime_streams.empty())
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {producers.size() - 1}});
            for (auto slot : linked.program_snapshots)
            {
                const auto source = source_slots[slot];
                auto& snapshot = snapshots[source];
                if (!snapshot.program_input)
                {
                    if (!snapshot.source.ops->program_input)
                        return cxx::unexpected(RenderError{kStorageMismatch, {slot}});
                    snapshot.program_input = &snapshot.source.ops->program_input(snapshot.source.storage);
                }
                auto& selected = producer.program_sources;
                if (std::ranges::find(selected, source) == selected.end()) selected.push_back(source);
            }
            if (!producer.runtime_streams.empty())
            {
                producer.programs_dirty = true;
                program_producers.push_back(static_cast<std::uint32_t>(producers.size() - 1));
            }
            producer.constants.reserve(linked.inline_blocks.size());
            for (const auto& block : linked.inline_blocks)
            {
                producer.constants.push_back({{std::vector<std::byte>(block.bytes)}});
            }
        }
        cpu_data_.reserve(plan.cpu_data_bytes.size());
        for (auto bytes : plan.cpu_data_bytes) cpu_data_.push_back({std::vector<std::byte>(bytes)});
        prepared_buffer_inputs_.resize(plan.buffers.size());
        for (auto& frame : frames_)
        {
            frame.native.prepared_buffers.resize(plan.buffers.size());
        }
        producers_ = std::move(producers);
        program_producers_ = std::move(program_producers);
        if (!program_producers_.empty())
        {
            program_input_revision_ = 1;
            dynamic_raster_programs_.resize(plan_->passes().size());
        }
        // Build the Resource-to-kernel links while the execution is private. Publication only attaches rows.
        for (auto index : program_producers_)
        {
            auto& producer = producers_[index];
            for (auto slot : producer.program_sources)
            {
                auto* source = snapshots[slot].program_input;
                auto use = std::ranges::find(program_input_uses_, source, &ProgramInputRevision::Use::source);
                if (use == program_input_uses_.end())
                {
                    program_input_uses_.push_back({source, &program_input_revision_, {&producer.programs_dirty}});
                }
                else use->dirty_programs.push_back(&producer.programs_dirty);
            }
        }
        storage_snapshots_ = std::move(snapshots);
        snapshot_sources_ = std::move(source_slots);
        raster_owners_ = std::move(raster_owners);
        effect_values_ = std::move(inputs.effect_values);
        snapshot_registry_ = &registry;
        preparation_plan_ = &plan;
        return {};
    }

    void RenderGraphInstance::releaseDynamicRasterPrograms(std::uint32_t pass, bool replacing) noexcept
    {
        auto& selected = dynamic_raster_programs_[pass];
        if (!selected) return;
        if (replacing && !selected->programs.empty()) releaseRasterBindings(pass, selected->revision);
        const auto get = [&](std::size_t i) noexcept { return selected->programs[i].cache_id; };
        session_->programs().release(selected->programs.size(), get, gpu_.accepted);
        selected.reset();
    }

    bool RenderGraphInstance::hasExclusiveProgramInput() const noexcept
    {
        return std::ranges::any_of(storage_snapshots_, [&](const auto& snapshot) noexcept
        {
            return snapshot.program_input && snapshot_registry_->hasExclusiveCommand(snapshot.source.id);
        });
    }

    void RenderGraphInstance::installPreparation(RenderJobId job, std::uint64_t generation) noexcept
    {
        if (!generation || preparation_generation_) std::terminate();
        preparation_job_ = job;
        preparation_generation_ = generation;
        for (auto& use : program_input_uses_) use.source->attach(use);
    }

    RenderResult<void> RenderGraphInstance::selectProgramUpdate(RasterProgramUpdate& update) noexcept
    {
        update.input_revision = program_input_revision_;
        for (auto index : program_producers_)
        {
            if (!producers_[index].programs_dirty) continue;
            if (auto selected = buildRasterPrograms(index, update); !selected) return selected;
        }
        return {};
    }

    RenderResult<void>
    RenderGraphInstance::buildRasterPrograms(std::uint32_t index, RasterProgramUpdate& update) noexcept
    {
        auto& producer = producers_[index];
        for (auto slot : producer.program_sources)
        {
            auto& snapshot = storage_snapshots_[slot];
            auto captured = snapshot.source.ops->capture(snapshot.source.storage, snapshot.value.get());
            if (!captured) return cxx::unexpected(captured.error());
            if (*captured != snapshot.program_input->value())
                return cxx::unexpected(RenderError{kStorageContract, {slot}});
            snapshot.revision = *captured;
        }
        const auto& linked = preparation_plan_->producers[index];
        RasterProgramBuildData data{*snapshot_registry_, *session_};
        data.streams.reserve(producer.runtime_streams.size());
        for (auto pass : producer.runtime_streams)
        {
            auto& stream = data.streams.emplace_back(pass);
            stream.ready = bool(dynamic_raster_programs_[pass]);
            if (raster_owners_.empty()) continue;
            const auto& owners = raster_owners_[pass];
            if (owners.textures) stream.sources[0] = owners.textures->id();
            if (owners.meshes) stream.sources[1] = owners.meshes->id();
            if (owners.materials) stream.sources[2] = owners.materials->id();
        }
        data.snapshots.reserve(linked.program_snapshots.size());
        for (auto slot : linked.program_snapshots)
        {
            const auto& snapshot = storage_snapshots_[snapshot_sources_[slot]];
            data.snapshots.push_back({
                slot, snapshot.source.id, snapshot.source.ops->type, {snapshot.value.get(), snapshot.revision}
            });
        }
        RasterProgramBuildContext context{data};
        auto mapping = linked.build_programs(linked.prototype.inputs().data(), context, producer.draw_mapping.get());
        if (!mapping) return cxx::unexpected(mapping.error());
        update.selections.push_back({index, std::move(*mapping)});
        for (const auto& entry : data.streams)
        {
            if (entry.reuse)
            {
                continue;
            }
            if (!entry.programs) return cxx::unexpected(RenderError{kFrameContract, {entry.pass}});
            const auto& contract = *programs_->rasterStream(entry.pass)->runtime;
            auto& stream = update.streams.emplace_back();
            stream.pass = entry.pass;
            stream.candidate = std::make_unique<DynamicRasterPrograms>();
            const auto& previous = dynamic_raster_programs_[entry.pass];
            if (previous && previous->revision == UINT64_MAX)
                return cxx::unexpected(RenderError{kDeviceCapacity});
            stream.candidate->revision = previous ? previous->revision + 1 : 1;
            stream.candidate->programs.reserve(entry.programs->size());
            stream.programs.reserve(entry.programs->size());
            for (const auto& program : *entry.programs)
            {
                auto link = linkRasterStreamProgram(
                    *session_, {contract.arguments, program.parameters, contract.target,
                                entry.shaders[program.shader.index], program.graphics, program.geometry_locations}
                );
                if (!link) return cxx::unexpected(link.error());
                stream.programs.push_back({std::move(*link), program.source});
            }
        }
        return {};
    }

    void RenderGraphInstance::adoptProgramUpdate(RasterProgramUpdate& update) noexcept
    {
        for (auto& stream : update.streams)
        {
            auto& previous = dynamic_raster_programs_[stream.pass];
            if (previous && previous->programs == stream.candidate->programs) continue;
            releaseDynamicRasterPrograms(stream.pass, true);
            previous = std::move(stream.candidate);
        }
        for (auto& selection : update.selections)
        {
            auto& producer = producers_[selection.producer];
            producer.draw_mapping.swap(selection.draw_mapping);
            producer.programs_dirty = false;
        }
        applied_program_revision_ = update.input_revision;
    }

    RenderResult<void> RenderGraphInstance::prepareProducers(std::uint32_t slot) noexcept
    {
        if (!preparation_plan_) return {};
        if (needsProgramBuild()) return cxx::unexpected(RenderError{kDeviceBusy});
        if (slot >= frames_.size()) return cxx::unexpected(RenderError{kDeviceArgument});
        if (preparation_revision_ == UINT64_MAX) return cxx::unexpected(RenderError{kDeviceCapacity});
        for (const auto& snapshot : storage_snapshots_)
        {
            if (snapshot_registry_->hasExclusiveCommand(snapshot.source.id))
                return cxx::unexpected(RenderError{kDeviceBusy});
        }
        for (auto& snapshot : storage_snapshots_)
        {
            auto revision = snapshot.source.ops->capture(snapshot.source.storage, snapshot.value.get());
            if (!revision) return cxx::unexpected(revision.error());
            if (snapshot.program_input && *revision != snapshot.revision)
                return cxx::unexpected(RenderError{needsProgramBuild() ? kDeviceBusy : kStorageContract});
            snapshot.revision = *revision;
        }
        ++preparation_revision_;
        for (auto& data : cpu_data_) data.written = false;
        for (auto& buffer : prepared_buffer_inputs_) buffer.source = PreparedBufferInput::ESource::MISSING;

        const auto& linked_producers = preparation_plan_->producers;
        // Select each Effect-owned input once on the owner before invoking any producer.
        for (std::size_t i = 0; i < effect_values_.size(); ++i)
        {
            auto& value = cpu_data_[preparation_plan_->effect_inputs[i].destination.index];
            std::ranges::copy(effect_values_[i], value.bytes.begin());
            value.written = true;
        }
        for (std::size_t i = 0; i < linked_producers.size(); ++i)
        {
            const auto& linked = linked_producers[i];
            auto& producer = producers_[i];
            if (producer.programs_dirty) return cxx::unexpected(RenderError{kDeviceBusy});
            for (auto& block : producer.constants) block.payload.written = false;
            for (auto& stream : producer.streams)
            {
                stream.written = false;
                stream.textures.clear();
                stream.mesh_buffers.clear();
                stream.materials.clear();
                stream.material_images.clear();
                stream.selection = preparation_revision_;
            }
            ProducerFrame frame{*this, static_cast<std::uint32_t>(i), slot};
            if (auto prepared = producer.workspace.prepare(frame, producer.draw_mapping.get()); !prepared)
                return prepared;

            for (auto output : linked.outputs)
            {
                if (!cpu_data_[output].written)
                    return cxx::unexpected(RenderError{kGraphUndefinedRead, {kGraphInvalidIndex, output}});
            }
            for (auto output : linked.buffers)
            {
                if (prepared_buffer_inputs_[output].source == PreparedBufferInput::ESource::MISSING)
                    return cxx::unexpected(RenderError{
                        kGraphUndefinedRead, {kGraphInvalidIndex, preparation_plan_->buffers[output].resource}
                    });
            }
            for (std::size_t b = 0; b < linked.inline_blocks.size(); ++b)
            {
                if (!producer.constants[b].payload.written)
                    return cxx::unexpected(RenderError{kFrameContract, {linked.inline_blocks[b].pass}});
            }
            for (std::size_t s = 0; s < linked.raster_streams.size(); ++s)
            {
                if (!producer.streams[s].written)
                    return cxx::unexpected(RenderError{kFrameContract, {linked.raster_streams[s]}});
            }
        }

        if (needsProgramBuild()) return cxx::unexpected(RenderError{kDeviceBusy});
        // Commit the complete selection only after every preparation callback has succeeded.
        for (auto& replica : frames_[slot].native.prepared_buffers) replica.staged_bytes = 0;
        for (std::size_t i = 0; i < linked_producers.size(); ++i)
        {
            const auto& linked = linked_producers[i];
            for (std::size_t s = 0; s < linked.raster_streams.size(); ++s)
            {
                std::swap(frames_[slot].stream_inputs[linked.raster_streams[s]], producers_[i].streams[s]);
            }
            const auto& blocks = linked.inline_blocks;
            for (std::size_t b = 0; b < blocks.size(); ++b)
            {
                const auto& selected = producers_[i].constants[b];
                auto written = writeInlineConstant(
                    slot, blocks[b].pass, selected.payload.bytes, selected.source_revision
                );
                if (!written) return written;
                if (blocks[b].alternate != kGraphInvalidIndex)
                {
                    auto other = writeInlineConstant(slot, blocks[b].alternate, selected.payload.bytes, selected.source_revision);
                    if (!other) return other;
                }
            }
        }
        for (const auto& copy : preparation_plan_->constant_copies)
        {
            if (auto updated = writeInlineConstant(slot, copy.pass.index, cpu_data_[copy.input.index].bytes, 0); !updated)
                return updated;
        }
        return {};
    }

    RenderResult<bool> RenderGraphInstance::stagePreparedBuffers(
        std::uint32_t slot, ResourceUploader* uploads, std::vector<PreparedBufferCopy>& copies
    ) noexcept
    {
        auto& replicas = frames_[slot].native.prepared_buffers;
        for (std::size_t i = 0; i < replicas.size(); ++i)
        {
            const auto& contract = preparation_plan_->buffers[i];
            if (!contract.used) continue;
            if (contract.upload && !uploads) return cxx::unexpected(RenderError{kDeviceUnsupported});
            auto& replica = replicas[i];
            const auto& input = prepared_buffer_inputs_[i];
            const auto size = input.size;
            if (input.source == PreparedBufferInput::ESource::RESIDENT)
            {
                const bool same_allocation = replica.generation == input.resident_generation;
                if (!same_allocation || !replica.contains(input.revision, size))
                    return cxx::unexpected(RenderError{kFrameContract, {kGraphInvalidIndex, contract.resource}});
                setResource(contract.resource, slot, {replica.buffer, 0, size});
                continue;
            }
            if (size > replica.capacity)
            {
                const auto capacity = std::min(std::bit_ceil(size), contract.maximum_bytes);
                const auto description = bufferDescription(plan_->resources()[contract.resource], capacity);
                if (!description) return cxx::unexpected(description.error());
                nri::Buffer* candidate = nullptr;
                const auto result =
                    session_->nriCore().CreatePlacedBuffer(session_->device(), NriDeviceHeap, *description, candidate);
                if (result != nri::Result::SUCCESS) return cxx::unexpected(nativeError(result));
                setResource(contract.resource, slot, {candidate, 0, size});
                releaseAfterGpu(*session_, replica.buffer);
                replica.buffer = candidate;
                ++replica.generation;
                replica.capacity = capacity;
                replica.accepted_revision = 0;
                replica.accepted_size = 0;
            }
            if (!contract.upload)
            {
                setResource(contract.resource, slot, {replica.buffer, 0, size});
                continue;
            }
            const auto bytes = std::span<const std::byte>{input.bytes};
            if (replica.staged_bytes == size) continue;

            const auto first = uploads->stagedFrameCopyCount();
            const auto offset = replica.staged_bytes;
            auto staged = uploads->stageBufferPrefix({replica.buffer, offset, bytes.subspan(offset)});
            if (!staged) return cxx::unexpected(staged.error());
            if (*staged == 0) return false;
            replica.staged_bytes += *staged;
            copies.push_back({
                contract.producer,
                static_cast<std::uint32_t>(i),
                {first, uploads->stagedFrameCopyCount() - first},
                offset,
                size,
                input.revision
            });
            if (replica.staged_bytes != size) return false;
            setResource(contract.resource, slot, {replica.buffer, 0, size});
        }
        return true;
    }

    void RenderGraphInstance::settlePreparedBuffer(
        std::uint32_t slot, const PreparedBufferCopy& copy, SubmissionPoint accepted, RenderError error
    ) noexcept
    {
        auto& replica = frames_[slot].native.prepared_buffers[copy.buffer];
        if (preparation_plan_->buffers[copy.buffer].producer != copy.producer) std::terminate();
        if (error.type)
        {
            replica.staged_bytes = copy.offset;
            return;
        }
        if (!accepted.serial) std::terminate();
        gpu_.accepted.accept(accepted);
        // An accepted prefix can overwrite bytes of the previous revision.
        const bool complete = replica.staged_bytes == copy.size;
        replica.accepted_revision = complete ? copy.revision : 0;
        replica.accepted_size = complete ? copy.size : 0;
    }
} // namespace lux::render::detail
