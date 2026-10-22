#include <lux/engine/render/resource/StorageSet.hpp>
#include <algorithm>
#include <bit>
#include <cstring>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/graph/RenderPlan.hpp>
#include <lux/engine/render/memory/Cleanup.hpp>
#include <lux/engine/render/memory/DirtyRanges.hpp>
#include <lux/engine/render/storage/RasterMaterialTableStorage.hpp>
#include <lux/engine/render/storage/SampledTexture2DStorage.hpp>

namespace lux::render::detail
{
    struct RasterMaterialTableStorage::Table final
    {
        rdesc::MaterialProgramSet program;
        std::uint32_t stride = 0;
        std::vector<std::uint32_t> parameter_offsets, texture_offsets;
        std::vector<std::byte> records; // Adopted GPU-format bytes used to fill and repair every FIF replica.
        std::vector<Entry*> entries;
        std::vector<std::uint32_t> free_slots;

        struct GpuSlice final
        {
            nri::Buffer* parameters = nullptr;
            nri::Buffer* pending_parameters = nullptr;
            std::uint32_t pending_capacity = 0;
            std::uint32_t capacity = 0;
            DirtyRanges dirty;
            std::uint32_t position = UINT32_MAX;
            std::vector<DirtyWordMask> selected, accepted;
            std::uint32_t partial_record = UINT32_MAX, next_partial_record = UINT32_MAX;
            std::size_t partial_bytes = 0, next_partial_bytes = 0;
        };

        std::array<GpuSlice, 3> gpu;
        std::vector<std::byte> value_bytes; // One writer's reflected record, never a copy of the whole table.
        bool updating = false;
    };

    RasterMaterialTableStorage::RasterMaterialTableStorage(StorageCreateContext& dependencies, StorageId id) noexcept
        : session_(dependencies.device), uploads_(*dependencies.uploads), frame_execution_(*dependencies.frames),
          resources_(dependencies.resources), id_(id)
    {
    }

    RasterMaterialTableStorage::~RasterMaterialTableStorage() noexcept
    {
        for (auto& entry : entries_)
        {
            StorageSubscription::disconnect(entry.bindings);
        }
        for (auto& table : tables_)
        {
            releaseTable(*table);
        }
    }

    std::size_t RasterMaterialTableStorage::recordCount() const noexcept
    {
        return entries_.size();
    }

    RenderResult<RasterMaterialTableStorage::Table*> RasterMaterialTableStorage::table(
        rdesc::MaterialProgramSet& program
    ) noexcept
    {
        for (auto& current : tables_)
        {
            if (current->program == program)
            {
                return current.get();
            }
        }
        const auto* layout = program.parameterLayout();
        const bool is_invalid_layout = !layout || !layout->runtime_records || !layout->size || layout->size % 4;
        const bool is_invalid_shader =
            program.programs.empty() ||
            std::ranges::any_of(
                program.programs,
                [&](const auto& use)
                {
                    const auto& stages = use.shader.stages;
                    const bool invalid_code = stages.size() != 1 ||
                                              stages.front().stage != rdesc::EShaderType::FRAGMENT ||
                                              stages.front().entry.empty() || stages.front().words.size() < 5 ||
                                              stages.front().words.front() != 0x07230203;
                    if (invalid_code)
                    {
                        return true;
                    }
                    for (const auto& set : use.shader.interface.sets)
                    {
                        for (const auto& binding : set.bindings)
                        {
                            if (binding.name == rdesc::kMaterialTableName && (!layout || binding.block != *layout))
                            {
                                return true;
                            }
                        }
                    }
                    return false;
                }
            );
        const bool empty_record = !layout && program.parameter_defaults.empty() && !program.texture_count;
        if ((!empty_record && is_invalid_layout) || is_invalid_shader)
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        auto candidate = std::make_unique<Table>();
        candidate->stride = layout ? layout->size : 4;
        const auto field = [&](std::string_view name, rdesc::EDataScalar scalar, std::uint32_t rows
                           ) -> const rdesc::ShaderMember*
        {
            const auto found = std::ranges::find(layout->fields, name, &rdesc::ShaderMember::path);
            if (found == layout->fields.end())
            {
                return nullptr;
            }
            const bool invalid = found->scalar != scalar || found->rows != rows || found->columns != 1 ||
                                 found->offset % 4 || found->offset > layout->size ||
                                 rows * 4 > layout->size - found->offset;
            return invalid ? nullptr : &*found;
        };
        for (std::size_t i = 0; i < program.parameter_defaults.size(); ++i)
        {
            const auto* parameter = field(
                std::string(rdesc::kMaterialParametersName) + "[" + std::to_string(i) + "]",
                rdesc::EDataScalar::FLOAT,
                4
            );
            if (!parameter)
            {
                return cxx::unexpected(RenderError{kStorageContract});
            }
            candidate->parameter_offsets.push_back(parameter->offset);
        }
        constexpr auto& members = rdesc::kMaterialTextureFields;
        for (std::uint32_t i = 0; i < program.texture_count; ++i)
        {
            std::uint32_t base = 0;
            for (std::uint32_t m = 0; m < members.size(); ++m)
            {
                const auto* member = field(
                    std::string(rdesc::kMaterialTexturesName) + "[" + std::to_string(i) + "]." +
                        std::string(members[m]),
                    rdesc::EDataScalar::UINT,
                    1
                );
                if (!member || (m && member->offset != base + m * 4))
                {
                    return cxx::unexpected(RenderError{kStorageContract});
                }
                if (!m)
                {
                    base = member->offset;
                }
            }
            candidate->texture_offsets.push_back(base);
        }
        candidate->program = std::move(program);
        auto* result = candidate.get();
        tables_.push_back(std::move(candidate));
        return result;
    }

    TextureGpuRef RasterMaterialTableStorage::resolveTextureReference(TextureId id) const noexcept
    {
        auto& registry = resources_;
        const auto* storage_set = registry.storageSet(id.storage);
        if (!storage_set || (storage_set != registry.storageSet(id_) && !storage_set->isGlobal()))
        {
            return {};
        }
        auto source = registry.borrow<SampledTexture2DStorage>(id.storage);
        const auto* texture = source ? (*source)->find(id) : nullptr;
        return texture ? texture->gpuReference() : TextureGpuRef{};
    }

    void RasterMaterialTableStorage::markDirty(Table& table, std::uint32_t frame, std::uint32_t record) noexcept
    {
        auto& slice = table.gpu[frame];
        slice.dirty.grow(table.records.size() / table.stride);
        slice.dirty.mark(record);
        if (slice.position == UINT32_MAX)
        {
            slice.position = static_cast<std::uint32_t>(dirty_tables_[frame].size());
            dirty_tables_[frame].push_back(&table);
        }
    }

    void RasterMaterialTableStorage::removeDirty(Table& table, std::uint32_t frame) noexcept
    {
        auto& position = table.gpu[frame].position;
        if (position == UINT32_MAX)
        {
            return;
        }
        auto& list = dirty_tables_[frame];
        list[position] = list.back();
        list[position]->gpu[frame].position = position;
        list.pop_back();
        position = UINT32_MAX;
    }

    void RasterMaterialTableStorage::releaseTable(Table& table) noexcept
    {
        for (std::uint32_t frame = 0; frame < frame_execution_.frameCount(); ++frame)
        {
            removeDirty(table, frame);
            releaseAfterGpu(session_, table.gpu[frame].parameters);
            releaseAfterGpu(session_, table.gpu[frame].pending_parameters);
        }
    }

    void RasterMaterialTableStorage::publish(Table& table, std::uint32_t frame) noexcept
    {
        const auto& buffer = table.gpu[frame].parameters;
        for (auto* entry : table.entries)
        {
            if (entry)
            {
                StorageSubscription::publish(
                    entry->bindings,
                    frame,
                    {buffer, 0, std::uint64_t(table.gpu[frame].capacity) * table.stride}
                );
            }
        }
    }

    template <class Input>
    exec::task<RenderResult<RasterMaterialTableStorage::WriteValue<Input>>> RasterMaterialTableStorage::write(
        Input input
    ) noexcept
    {
        Entry* existing = nullptr;
        Table* selected = nullptr;
        if constexpr (std::same_as<Input, MaterialWrite>)
        {
            if (input.storage != id_)
            {
                co_return cxx::unexpected(RenderError{kStorageIdentity});
            }
            auto found = table(input.program);
            if (!found)
            {
                co_return cxx::unexpected(found.error());
            }
            selected = *found;
        }
        else
        {
            existing = input.material.storage == id_ ? entries_.find(input.material.local) : nullptr;
            if (!existing)
            {
                co_return cxx::unexpected(RenderError{kStorageIdentity});
            }
            selected = existing->table;
        }
        auto& table = *selected;
        if (table.updating)
        {
            co_return cxx::unexpected(RenderError{kDeviceBusy});
        }
        table.updating = true;
        ++preparation_revision_;
        nri::Buffer* candidate = nullptr;
        cxx::scope_exit rollback(
            [&]() noexcept
            {
                table.updating = false;
                releaseAfterGpu(session_, candidate);
                if (table.entries.empty())
                {
                    const auto empty =
                        std::ranges::find_if(tables_, [&](const auto& value) { return value.get() == &table; });
                    releaseTable(table);
                    tables_.erase(empty);
                }
            }
        );
        const bool invalid_counts = input.parameters.size() != table.parameter_offsets.size() ||
                                    input.textures.size() != table.texture_offsets.size();
        if (invalid_counts)
        {
            co_return cxx::unexpected(RenderError{kStorageContract});
        }
        const auto slot = existing                    ? existing->slot
                          : !table.free_slots.empty() ? table.free_slots.back()
                                                      : static_cast<std::uint32_t>(table.entries.size());
        const auto current_capacity = table.records.size() / table.stride;
        const bool growing = slot >= current_capacity;
        const auto capacity =
            growing ? std::max<std::uint64_t>(16, std::bit_ceil(std::uint64_t(slot) + 1)) : current_capacity;
        const auto total_bytes = capacity * table.stride;
        if (total_bytes > session_.description().memory.storageBufferMaxRange || table.entries.size() >= UINT32_MAX)
        {
            co_return cxx::unexpected(RenderError{kStorageCapacity});
        }
        table.records.reserve(total_bytes);
        if constexpr (std::same_as<Input, MaterialWrite>)
        {
            entries_.reserve(std::bit_ceil(entries_.size() + 1));
            table.entries.reserve(capacity);
        }
        for (std::uint32_t frame = 0; frame < frame_execution_.frameCount(); ++frame)
        {
            table.gpu[frame].dirty.grow(capacity);
            dirty_tables_[frame].reserve(std::bit_ceil(tables_.size()));
        }
        table.value_bytes.assign(table.stride, std::byte{});
        auto values = std::span(table.value_bytes);
        for (std::size_t p = 0; p < input.parameters.size(); ++p)
        {
            std::memcpy(
                values.data() + table.parameter_offsets[p],
                input.parameters[p].data(),
                sizeof(input.parameters[p])
            );
        }
        // Resolve only the explicit request. Client retains these references through completion and
        // updates Materials before releasing Textures; no cross-Storage observer repairs later misuse.
        for (std::size_t t = 0; t < input.textures.size(); ++t)
        {
            const auto texture = resolveTextureReference(input.textures[t]);
            const MaterialTextureGpuRef reference{0, texture.image_index, texture.sampler_index, texture.flags};
            std::memcpy(values.data() + table.texture_offsets[t], &reference, sizeof(reference));
        }
        std::uint32_t frame_slot = 0;
        while (!frame_execution_.tryBeginWrite(frame_slot))
        {
            if (auto ready = co_await frame_execution_.whenWritable(values.size()); !ready)
            {
                co_return cxx::unexpected(ready.error());
            }
        }
        SubmissionFrontiers progress;
        cxx::scope_exit finish_frame([&]() noexcept { frame_execution_.finishWrite(frame_slot, progress); });
        auto& slice = table.gpu[frame_slot];
        const bool grow_gpu = slice.capacity < capacity;
        if (grow_gpu)
        {
            const nri::BufferDesc description{total_bytes, 4, nri::BufferUsageBits::SHADER_RESOURCE_STORAGE};
            const auto result =
                session_.nriCore().CreatePlacedBuffer(session_.device(), NriDeviceHeap, description, candidate);
            if (result != nri::Result::SUCCESS)
            {
                co_return cxx::unexpected(nativeError(result));
            }
        }
        auto& target = grow_gpu ? candidate : slice.parameters;
        cxx::scope_exit restore(
            [&]() noexcept
            {
                if (!grow_gpu)
                {
                    markDirty(table, frame_slot, slot);
                }
            }
        );
        std::array<BufferWrite, 2> writes;
        std::size_t write_count = 0;
        if (grow_gpu && !table.records.empty())
        {
            writes[write_count++] = {target, 0, table.records};
        }
        writes[write_count++] = {target, std::uint64_t(slot) * table.stride, values};
        const BufferZero fill{target, 0, total_bytes};
        auto uploaded = co_await uploads_.writeBuffers(
            std::span(writes).first(write_count),
            grow_gpu ? std::span(&fill, 1) : std::span<const BufferZero>{},
            frame_slot,
            &progress
        );
        if (!uploaded)
        {
            co_return cxx::unexpected(uploaded.error());
        }
        if (growing)
        {
            table.records.resize(total_bytes);
        }
        std::memcpy(table.records.data() + std::size_t(slot) * table.stride, values.data(), values.size());
        for (std::uint32_t frame = 0; frame < frame_execution_.frameCount(); ++frame)
        {
            if (frame != frame_slot)
            {
                markDirty(table, frame, slot);
            }
        }
        if (grow_gpu)
        {
            releaseAfterGpu(session_, slice.pending_parameters);
            slice.pending_capacity = 0;
            releaseAfterGpu(session_, slice.parameters);
            slice.parameters = std::exchange(candidate, nullptr);
            slice.capacity = static_cast<std::uint32_t>(capacity);
            slice.dirty.clear(); // The actual growth copy covered the complete CPU table plus this record.
            slice.partial_record = UINT32_MAX;
            slice.partial_bytes = 0;
            removeDirty(table, frame_slot);
        }
        else
        {
            slice.dirty.grow(capacity);
            slice.dirty.acknowledge({slot / 64, std::uint64_t{1} << (slot % 64)});
        }
        restore.release();
        MaterialId id;
        if constexpr (std::same_as<Input, MaterialWrite>)
        {
            id = {id_, entries_.emplace()};
            existing = entries_.find(id.local);
            existing->table = &table;
            existing->slot = slot;
            if (!table.free_slots.empty())
            {
                table.free_slots.pop_back();
            }
            if (slot == table.entries.size())
            {
                table.entries.push_back(existing);
            }
            else
            {
                table.entries[slot] = existing;
            }
        }
        if (grow_gpu)
        {
            publish(table, frame_slot);
        }
        if constexpr (std::same_as<Input, MaterialWrite>)
        {
            co_return id;
        }
        else
        {
            co_return Ack{};
        }
    }

    exec::task<RenderResult<MaterialId>> RasterMaterialTableStorage::upload(MaterialWrite input) noexcept
    {
        return write(std::move(input));
    }

    exec::task<RenderResult<Ack>> RasterMaterialTableStorage::upload(MaterialUpdate input) noexcept
    {
        return write(std::move(input));
    }

    RenderResult<std::uint32_t> RasterMaterialTableStorage::gpuIndex(MaterialId id) const noexcept
    {
        const auto* entry = id.storage == id_ ? entries_.find(id.local) : nullptr;
        if (!entry)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        return entry->slot;
    }

    bool RasterMaterialTableStorage::sameTable(MaterialId left, MaterialId right) const noexcept
    {
        const auto* a = left.storage == id_ ? entries_.find(left.local) : nullptr;
        const auto* b = right.storage == id_ ? entries_.find(right.local) : nullptr;
        return a && b && a->table == b->table;
    }

    RenderResult<void> RasterMaterialTableStorage::release(MaterialId id) noexcept
    {
        if (frame_execution_.submitting())
        {
            return cxx::unexpected(RenderError{kDeviceBusy});
        }
        auto* entry = id.storage == id_ ? entries_.find(id.local) : nullptr;
        if (!entry)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        if (entry->table->updating)
        {
            return cxx::unexpected(RenderError{kDeviceBusy});
        }
        auto& table = *entry->table;
        StorageSubscription::disconnect(entry->bindings);
        table.entries[entry->slot] = nullptr;
        table.free_slots.push_back(entry->slot);
        ++preparation_revision_;
        entries_.erase(id.local);
        if (table.free_slots.size() == table.entries.size())
        {
            releaseTable(table);
            const auto empty = std::ranges::find_if(tables_, [&](const auto& value) { return value.get() == &table; });
            tables_.erase(empty);
        }
        return {};
    }

    RenderResult<void> RasterMaterialTableStorage::resolve(
        const VGraphResourceDesc& plan,
        const MaterialBinding& argument,
        StorageSubscription& subscription,
        std::span<ResourceInput> output
    ) noexcept
    {
        auto* entry = argument.material.storage == id_ ? entries_.find(argument.material.local) : nullptr;
        if (!entry)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        const auto& table = *entry->table;
        if (argument.texture_slot != UINT32_MAX)
        {
            const bool invalid_slot = argument.texture_slot >= table.texture_offsets.size();
            if (!std::holds_alternative<GraphImageDesc>(plan) || invalid_slot)
            {
                return cxx::unexpected(RenderError{kStorageMismatch});
            }
            // This Graph port selects the device bindless table, not one physical Image. Its
            // Texture indices were resolved by the explicit write; no Client asset mirror is kept.
            std::ranges::fill(output, ResourceInput{});
            return {};
        }
        if (argument.layout != *table.program.parameterLayout())
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        const auto* description = std::get_if<GraphBufferDesc>(&plan);
        if (!description || description->size < table.stride || description->size > table.records.size())
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        subscription.attach(entry->bindings);
        for (std::uint32_t frame = 0; frame < output.size(); ++frame)
        {
            const auto& buffer = table.gpu[frame].parameters;
            output[frame] =
                {buffer, 0, buffer ? std::uint64_t(table.gpu[frame].capacity) * table.stride : table.records.size()};
        }
        return {};
    }

    RenderResult<bool> RasterMaterialTableStorage::prepareFrame(std::uint32_t frame, ResourceUploader& uploads) noexcept
    {
        bool copied_any = false;
        cxx::scope_exit acknowledge(
            [&]() noexcept
            {
                if (copied_any)
                {
                    frame_execution_.onPreparationSubmitted(
                        this,
                        [](void* value, std::uint32_t frame, RenderResult<void> accepted) noexcept
                        {
                            auto& storage = *static_cast<RasterMaterialTableStorage*>(value);
                            storage.finishFramePreparation(frame, accepted);
                        }
                    );
                }
            }
        );
        bool ready = true;
        auto& tables = dirty_tables_[frame];
        for (std::size_t i = 0; i < tables.size();)
        {
            auto& table = *tables[i];
            auto& slice = table.gpu[frame];
            if (slice.dirty.empty())
            {
                removeDirty(table, frame);
                continue;
            }
            ++i;
            const auto capacity = static_cast<std::uint32_t>(table.records.size() / table.stride);
            if (std::max(slice.capacity, slice.pending_capacity) < capacity)
            {
                const nri::BufferDesc description{
                    table.records.size(),
                    4,
                    nri::BufferUsageBits::SHADER_RESOURCE_STORAGE
                };
                nri::Buffer* buffer = nullptr;
                const auto result =
                    session_.nriCore().CreatePlacedBuffer(session_.device(), NriDeviceHeap, description, buffer);
                if (result != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(result));
                }
                releaseAfterGpu(session_, slice.pending_parameters);
                slice.pending_parameters = buffer;
                slice.pending_capacity = capacity;
                slice.dirty.grow(capacity);
                slice.dirty.markRange(0, capacity);
                slice.partial_record = UINT32_MAX;
                slice.partial_bytes = 0;
            }
            slice.accepted.clear();
            slice.next_partial_record = slice.partial_record;
            slice.next_partial_bytes = slice.partial_bytes;
            std::uint64_t covered = 0;
            bool progress = false;
            const auto stage = [&](std::uint32_t first, std::uint32_t count, std::size_t offset
                               ) noexcept -> RenderResult<bool>
            {
                while (count)
                {
                    const auto begin = std::size_t(first) * table.stride + offset;
                    const auto source = std::span<const std::byte>(table.records)
                                            .subspan(begin, std::size_t(count) * table.stride - offset);
                    auto* target = slice.pending_parameters ? slice.pending_parameters : slice.parameters;
                    auto copied = uploads.stageBuffer({target, begin, source});
                    if (!copied)
                    {
                        return cxx::unexpected(copied.error());
                    }
                    if (*copied == 0)
                    {
                        return false;
                    }
                    progress = true;
                    const auto total = offset + *copied;
                    const auto records = static_cast<std::uint32_t>(total / table.stride);
                    if (records)
                    {
                        DirtyRanges::appendRange(slice.accepted, first, records);
                        covered += records;
                    }
                    first += records;
                    count -= records;
                    offset = total % table.stride;
                    slice.next_partial_record = offset ? first : UINT32_MAX;
                    slice.next_partial_bytes = offset;
                }
                return true;
            };
            RenderResult<bool> staged = true;
            if (slice.partial_record != UINT32_MAX)
            {
                staged = stage(slice.partial_record, 1, slice.partial_bytes);
            }
            if (staged && *staged)
            {
                const auto word_limit =
                    (uploads.windowByteLimit() / table.stride + 63) / 64 + uploads.windowRegionLimit();
                slice.dirty.select(word_limit, slice.selected);
                if (slice.partial_record != UINT32_MAX)
                {
                    const auto partial_word = slice.partial_record / 64;
                    const auto found = std::ranges::find(slice.selected, partial_word, &DirtyWordMask::word_index);
                    if (found != slice.selected.end())
                    {
                        found->bits &= ~(std::uint64_t{1} << (slice.partial_record % 64));
                    }
                }
                const bool complete = DirtyRanges::visitRanges(
                    slice.selected,
                    UINT32_MAX,
                    [&](std::uint32_t first, std::uint32_t count) noexcept
                    {
                        staged = stage(first, count, 0);
                        return staged && *staged;
                    }
                );
                if (staged)
                {
                    staged = complete;
                }
            }
            copied_any |= progress;
            if (!staged)
            {
                return cxx::unexpected(staged.error());
            }
            ready &= covered == slice.dirty.recordCount() && !slice.pending_parameters;
        }
        return ready;
    }

    void RasterMaterialTableStorage::finishFramePreparation(std::uint32_t frame, RenderResult<void> accepted) noexcept
    {
        auto& dirty = dirty_tables_[frame];
        for (std::size_t index = 0; index < dirty.size();)
        {
            auto& table = *dirty[index];
            auto& slice = table.gpu[frame];
            if (accepted)
            {
                slice.dirty.acknowledge(slice.accepted);
                slice.partial_record = slice.next_partial_record;
                slice.partial_bytes = slice.next_partial_bytes;
            }
            slice.accepted.clear();
            if (accepted && slice.dirty.empty())
            {
                if (slice.pending_parameters)
                {
                    releaseAfterGpu(session_, slice.parameters);
                    slice.parameters = std::exchange(slice.pending_parameters, nullptr);
                    slice.capacity = std::exchange(slice.pending_capacity, 0);
                    publish(table, frame);
                }
                removeDirty(table, frame);
            }
            else
            {
                ++index;
            }
        }
    }

    StorageFactory RasterMaterialTableStorage::factory() noexcept
    {
        static const StorageBindingOps binding{
            [](void* object,
               const VGraphResourceDesc& plan,
               const void* argument,
               StorageSubscription& subscription,
               std::span<ResourceInput> output) noexcept
            {
                return static_cast<RasterMaterialTableStorage*>(object)
                    ->resolve(plan, *static_cast<const MaterialBinding*>(argument), subscription, output);
            },
            [](void* object, std::uint32_t slot, ResourceUploader& uploads) noexcept
            { return static_cast<RasterMaterialTableStorage*>(object)->prepareFrame(slot, uploads); },
            [](const void* value) noexcept
            { return &static_cast<const RasterMaterialTableStorage*>(value)->preparation_revision_; }
        };
        static const auto create = +[](StorageCreateContext& dependencies, StorageId id
                                    ) noexcept -> RenderResult<std::unique_ptr<RasterMaterialTableStorage>>
        {
            if (!dependencies.uploads || !dependencies.frames)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            return std::make_unique<RasterMaterialTableStorage>(dependencies, id);
        };
        return makeStorageFactory<
            render::RasterMaterialTableStorageDescriptor,
            RasterMaterialTableStorage,
            StorageCreateContext,
            create>(nullptr, &binding);
    }
} // namespace lux::render::detail

namespace lux::render
{
    exec::task<RenderResult<MaterialId>> StorageTraits<detail::RasterMaterialTableStorage>::upload(
        detail::RasterMaterialTableStorage& storage,
        MaterialWrite input
    ) noexcept
    {
        return storage.upload(std::move(input));
    }

    exec::task<RenderResult<Ack>> StorageTraits<detail::RasterMaterialTableStorage>::upload(
        detail::RasterMaterialTableStorage& storage,
        MaterialUpdate input
    ) noexcept
    {
        return storage.upload(std::move(input));
    }
} // namespace lux::render
