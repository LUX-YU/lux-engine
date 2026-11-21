#include <lux/engine/render/extensions/resource/ReplicaMutation.hpp>
#include <lux/engine/render/extensions/FrameDataCall.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/graph/RenderGraphExecutable.hpp>
#include <lux/engine/render/memory/Cleanup.hpp>
#include <lux/engine/render/memory/DirtyRanges.hpp>
#include <lux/engine/render/resource/ResourceSet.hpp>
#include <lux/engine/render/resource/material/RasterMaterialStorage.hpp>
#include <lux/engine/render/resource/texture/SampledTexture2DStorage.hpp>
#include <utility>

namespace lux::render::detail
{
    namespace
    {
        exec::task<MaterialCommands::Update::Response>
        updateMaterial(
            RasterMaterialStorage& resource,
            ReplicaMutation& mutation,
            MaterialCommands::Update::Request&& input
        ) noexcept
        {
            auto updated = co_await resource.upload(mutation, std::move(input.data));
            co_return MaterialCommands::Update::Response{updated ? RenderError{} : updated.error()};
        }

        RenderResult<void>
        applyMaterialColors(RasterMaterialStorage& storage, std::span<const MaterialColorUpdate> updates) noexcept
        {
            return storage.updateColors(updates);
        }

        void updateMaterialColors(
            RasterMaterialStorage& storage, const MaterialCommands::UpdateColor::Request& input, AckResponse& reply
        ) noexcept
        {
            const auto updated = applyMaterialColors(storage, input.updates);
            reply.error = updated ? RenderError{} : updated.error();
        }

        exec::task<MaterialCommands::RegisterProgram::Response>
        handleRegisterProgram(RasterMaterialStorage& resource, MaterialCommands::RegisterProgram::Request&& input) noexcept
        {
            const auto registered = resource.registerProgram(std::move(input.program));
            co_return registered ? MaterialCommands::RegisterProgram::Response{{}, *registered}
                                   : MaterialCommands::RegisterProgram::Response{registered.error()};
        }

        exec::task<MaterialCommands::CreateInstance::Response>
        handleCreateInstance(
            RasterMaterialStorage& resource,
            ReplicaMutation& mutation,
            MaterialCommands::CreateInstance::Request&& input
        ) noexcept
        {
            const auto created = co_await resource.createInstance(mutation, std::move(input));
            co_return created ? MaterialCommands::CreateInstance::Response{{}, *created}
                              : MaterialCommands::CreateInstance::Response{created.error()};
        }

        void releaseInstance(
            RasterMaterialStorage& resource, const MaterialCommands::ReleaseInstance::Request& input,
            MaterialCommands::ReleaseInstance::Response& response
        ) noexcept
        {
            const auto released = resource.release(input.material);
            response.error = released ? RenderError{} : released.error();
        }

        void handleUnregisterProgram(
            RasterMaterialStorage& resource, const MaterialCommands::UnregisterProgram::Request& input,
            MaterialCommands::UnregisterProgram::Response& response
        ) noexcept
        {
            const auto removed = resource.unregisterProgram(input.program);
            response.error = removed ? RenderError{} : removed.error();
        }

    } // namespace

    struct RasterMaterialStorage::Table final
    {
        rdesc::ShaderBlock layout;
        std::array<std::optional<rdesc::EDescriptorBindingInfo>, 3> bindings;
        std::uint32_t shader_abi = rdesc::kMaterialShaderAbi;
        std::size_t digest = 0;
        std::size_t programs = 0;
        std::uint32_t stride = 0;
        std::vector<std::uint32_t> parameter_offsets, texture_offsets;
        std::vector<std::unique_ptr<Page>> pages;

        [[nodiscard]] bool hasSameAbi(const Table& other) const noexcept
        {
            return stride == other.stride && shader_abi == other.shader_abi && layout == other.layout &&
                parameter_offsets == other.parameter_offsets && texture_offsets == other.texture_offsets &&
                bindings == other.bindings;
        }
    };

    struct RasterMaterialStorage::Page final
    {
        Table& definition;
        std::uint32_t capacity;
        std::vector<std::byte> records;
        std::vector<std::uint32_t> free_slots;

        struct GpuSlice final
        {
            nri::Buffer* parameters = nullptr;
            nri::Descriptor* descriptor = nullptr;
            DescriptorTable::Slot slot;
            DirtyRanges dirty;
            std::uint32_t position = UINT32_MAX;
            std::vector<DirtyWordMask> selected, accepted;
        };

        std::array<GpuSlice, 3> gpu;
        std::vector<std::byte> value_bytes;
        bool updating = false;
    };

    RasterMaterialStorage::RasterMaterialStorage(ResourceCreateContext& dependencies, ResourceInstanceId id) noexcept
        : session_(dependencies.device), has_uploads_(dependencies.uploads != nullptr), frame_count_(dependencies.frame_count),
          resources_(dependencies.resources), id_(id)
    {
    }

    RasterMaterialStorage::~RasterMaterialStorage() noexcept
    {
        for (auto& entry : entries_)
        {
            StorageSubscription::disconnect(entry.bindings);
        }
        for (auto& [digest, table] : tables_)
        {
            for (auto& page : table->pages)
            {
                releasePage(*page);
            }
        }
    }

    std::size_t RasterMaterialStorage::recordCount() const noexcept
    {
        return entries_.size();
    }

    RenderResult<MaterialProgramId> RasterMaterialStorage::registerProgram(rdesc::MaterialProgramSet program) noexcept
    {
        if (program.target != session_.enabledFeatures().shader_target)
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        const auto* layout = program.parameterLayout();
        const bool empty_record = program.hasEmptyRecord();
        const bool is_invalid_layout = !layout || !layout->hasStructuredLayout() || !layout->runtime_records ||
                                       layout->size % 4 != 0 || layout->size % layout->alignment != 0;
        const bool is_invalid_state =
            program.shading_model > rdesc::ELightingTechnique::GRAPH || program.alpha_mode > rdesc::EAlphaMode::BLEND;
        const bool is_invalid_program =
            (!empty_record && is_invalid_layout) || program.programs.empty() || is_invalid_state;
        if (is_invalid_program)
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        if (layout)
        {
            constexpr auto required = static_cast<std::uint32_t>(EStorageCapability::STORAGE_BUFFER_ARRAY);
            const bool has_record_support = (session_.resourceCapabilities() & required) != 0;
            if (!has_record_support) return cxx::unexpected(RenderError{kDeviceUnsupported});
            if (!has_uploads_) return cxx::unexpected(RenderError{kDeviceArgument});
        }
        std::array<bool, 3> purposes{};
        std::array<std::optional<rdesc::EDescriptorBindingInfo>, 3> bindings;
        bool has_record_consumer = false;
        for (const auto& use : program.programs)
        {
            const auto purpose = static_cast<std::size_t>(use.purpose);
            const bool is_invalid_purpose = purpose >= purposes.size() || purposes[purpose];
            const auto& stages = use.shader.stages;
            const bool is_invalid_code = use.shader.target != program.target || stages.size() != 1 ||
                                         stages.front().stage != rdesc::EShaderType::FRAGMENT ||
                                         stages.front().entry.empty() ||
                                         !rdesc::validShaderContainer(use.shader.target, stages.front().words);
            if (is_invalid_purpose || is_invalid_code)
            {
                return cxx::unexpected(RenderError{kStorageContract});
            }
            purposes[purpose] = true;
            const auto& entries = use.shader.interface.entry_points;
            const bool is_invalid_entry = entries.size() != 1 || entries.front().name != stages.front().entry ||
                                          entries.front().stage != stages.front().stage;
            if (is_invalid_entry)
            {
                return cxx::unexpected(RenderError{kStorageContract});
            }
            bool has_table = false;
            for (const auto& set : use.shader.interface.sets)
            {
                for (const auto& binding : set.bindings)
                {
                    if (binding.name != rdesc::kMaterialTableName)
                    {
                        continue;
                    }
                    const bool is_invalid_binding =
                        has_table || !layout || binding.block != *layout || binding.count != 0 ||
                        binding.type != rdesc::EDescriptorType::STORAGE_BUFFER || binding.writable || !binding.readable;
                    if (is_invalid_binding)
                    {
                        return cxx::unexpected(RenderError{kStorageContract});
                    }
                    has_table = true;
                    bindings[purpose] = binding;
                }
            }
            has_record_consumer |= has_table;
        }
        if (has_record_consumer == empty_record)
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        Table* table = nullptr;
        if (layout)
        {
            auto resolved = qualifyTable(program, *layout, std::move(bindings));
            if (!resolved) return cxx::unexpected(resolved.error());
            table = *resolved;
        }
        const auto key = programs_.emplace();
        const auto id = extension_detail::ResourceIdentity::make<MaterialProgramId>(id_, key);
        auto* slot = programs_.find(key);
        slot->record = {id, std::move(program)};
        slot->table = table;
        if (table) ++table->programs;
        return id;
    }

    RenderResult<RasterMaterialStorage::Table*> RasterMaterialStorage::qualifyTable(
        const rdesc::MaterialProgramSet& program, const rdesc::ShaderBlock& layout,
        std::array<std::optional<rdesc::EDescriptorBindingInfo>, 3> bindings
    ) noexcept
    {
        auto candidate = std::make_unique<Table>();
        candidate->stride = layout.size;
        const auto field = [&](std::string_view name, rdesc::EDataScalar scalar, std::uint32_t rows
                           ) -> const rdesc::ShaderMember*
        {
            const auto found = std::ranges::find(layout.fields, name, &rdesc::ShaderMember::path);
            if (found == layout.fields.end())
            {
                return nullptr;
            }
            const bool invalid = found->scalar != scalar || found->rows != rows || found->columns != 1 ||
                                 found->offset % 4 || found->offset > layout.size ||
                                 rows * 4 > layout.size - found->offset;
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
                    const bool is_invalid_record = layout.size < sizeof(rdesc::MaterialTextureGpuRef) ||
                                                   base > layout.size - sizeof(rdesc::MaterialTextureGpuRef);
                    if (is_invalid_record)
                    {
                        return cxx::unexpected(RenderError{kStorageContract});
                    }
                }
            }
            candidate->texture_offsets.push_back(base);
        }
        std::vector<std::pair<std::uint32_t, std::uint32_t>> fields;
        fields.reserve(candidate->parameter_offsets.size() + candidate->texture_offsets.size());
        for (auto offset : candidate->parameter_offsets)
        {
            fields.emplace_back(offset, 16);
        }
        for (auto offset : candidate->texture_offsets)
        {
            fields.emplace_back(offset, static_cast<std::uint32_t>(sizeof(rdesc::MaterialTextureGpuRef)));
        }
        std::ranges::sort(fields);
        for (std::size_t i = 1; i < fields.size(); ++i)
        {
            if (fields[i].first < fields[i - 1].first + fields[i - 1].second)
            {
                return cxx::unexpected(RenderError{kStorageContract});
            }
        }
        candidate->layout = layout;
        candidate->bindings = std::move(bindings);
        // This digest only narrows candidates. Full reflection and projection settle equivalence.
        const auto digest = (std::size_t(layout.size) * 0x9e3779b9u) ^ layout.alignment ^ layout.fields.size();
        const auto [first, last] = tables_.equal_range(digest);
        for (auto current = first; current != last; ++current)
        {
            if (current->second->hasSameAbi(*candidate)) return current->second.get();
        }
        auto* result = candidate.get();
        candidate->digest = digest;
        tables_.emplace(digest, std::move(candidate));
        return result;
    }

    void RasterMaterialStorage::removeProgram(ProgramSlot& program) noexcept
    {
        auto* table = program.table;
        const auto key = extension_detail::ResourceIdentity::local(program.record.id);
        programs_.erase(key);
        if (table)
        {
            --table->programs;
            removeEmptyTable(*table);
        }
    }

    RenderResult<void> RasterMaterialStorage::unregisterProgram(MaterialProgramId id) noexcept
    {
        auto* program = findProgram(id);
        if (!program) return cxx::unexpected(RenderError{kStorageIdentity});
        const bool has_live_uses = program->instances || program->program_preparations != 0;
        if (has_live_uses) return cxx::unexpected(RenderError{kDeviceBusy});
        removeProgram(*program);
        return {};
    }

    void RasterMaterialStorage::removeEmptyTable(Table& table) noexcept
    {
        if (table.programs || !table.pages.empty()) return;
        const auto [first, last] = tables_.equal_range(table.digest);
        for (auto item = first; item != last; ++item)
        {
            if (item->second.get() != &table) continue;
            tables_.erase(item);
            return;
        }
        std::terminate();
    }

    RenderResult<RasterMaterialStorage::TextureInput> RasterMaterialStorage::resolveTexture(TextureId id) const noexcept
    {
        if (id.isNull())
        {
            return TextureInput{};
        }
        const auto parent = extension_detail::ResourceIdentity::parent(id);
        if (!resources_.canReference(id_, parent))
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        auto source = resources_.borrow<SampledTexture2DStorage>(parent);
        const auto* texture = source ? (*source)->find(id) : nullptr;
        if (!texture)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        return TextureInput{texture->gpuReference(), &texture->image()};
    }

    RenderResult<RasterMaterialStorage::Page*> RasterMaterialStorage::createPage(Table& table) noexcept
    {
        const auto maximum = session_.description().memory.storageBufferMaxRange;
        if (table.stride > maximum)
        {
            return cxx::unexpected(RenderError{kStorageCapacity});
        }
        const auto page_bytes = std::min<std::uint64_t>(4u * 1024u * 1024u, maximum);
        const auto max_records = std::max<std::uint64_t>(1, std::bit_floor(page_bytes / table.stride));
        const auto requested = table.pages.empty() ? 16u : std::uint64_t(table.pages.back()->capacity) * 2;
        const auto capacity = static_cast<std::uint32_t>(std::min(requested, max_records));
        auto page = std::unique_ptr<Page>(new Page{table, capacity});
        page->records.resize(std::size_t(capacity) * table.stride);
        page->free_slots.reserve(capacity);
        for (auto i = capacity; i != 0; --i)
        {
            page->free_slots.push_back(i - 1);
        }
        cxx::scope_exit rollback([&]() noexcept { releasePage(*page); });
        const auto& core = session_.nriCore();
        for (std::uint32_t frame = 0; frame < frame_count_; ++frame)
        {
            auto& slice = page->gpu[frame];
            slice.dirty.grow(capacity);
            const nri::BufferDesc description{
                page->records.size(),
                table.stride,
                (nri::BufferUsageBits::SHADER_RESOURCE | nri::BufferUsageBits::SHADER_RESOURCE_STORAGE)
            };
            auto status = core.CreatePlacedBuffer(session_.device(), NriDeviceHeap, description, slice.parameters);
            if (status != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(status));
            }
            const nri::BufferViewDesc
                view{slice.parameters, nri::BufferView::STRUCTURED_BUFFER, 0, page->records.size()};
            status = core.CreateBufferView(view, slice.descriptor);
            if (status != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(status));
            }
            auto slot = session_.descriptors().allocate(nri::DescriptorType::STRUCTURED_BUFFER);
            if (!slot)
            {
                return cxx::unexpected(slot.error());
            }
            slice.slot = *slot;
            session_.descriptors().write(nri::DescriptorType::STRUCTURED_BUFFER, *slot, slice.descriptor);
        }
        auto* result = page.get();
        rollback.release();
        table.pages.push_back(std::move(page));
        return result;
    }

    void RasterMaterialStorage::markDirty(Page& page, std::uint32_t frame, std::uint32_t record) noexcept
    {
        auto& slice = page.gpu[frame];
        if (!slice.parameters)
        {
            return;
        }
        slice.dirty.mark(record);
        if (slice.position == UINT32_MAX)
        {
            slice.position = static_cast<std::uint32_t>(dirty_pages_[frame].size());
            dirty_pages_[frame].push_back(&page);
        }
    }

    void RasterMaterialStorage::removeDirty(Page& page, std::uint32_t frame) noexcept
    {
        auto& position = page.gpu[frame].position;
        if (position == UINT32_MAX)
        {
            return;
        }
        auto& list = dirty_pages_[frame];
        list[position] = list.back();
        list[position]->gpu[frame].position = position;
        list.pop_back();
        position = UINT32_MAX;
    }

    void RasterMaterialStorage::releasePage(Page& page) noexcept
    {
        for (std::uint32_t frame = 0; frame < frame_count_; ++frame)
        {
            removeDirty(page, frame);
            auto& slice = page.gpu[frame];
            auto* buffer = std::exchange(slice.parameters, nullptr);
            auto* view = std::exchange(slice.descriptor, nullptr);
            auto slot = std::exchange(slice.slot, {});
            if (!buffer && !view && slot.isNull())
            {
                continue;
            }
            session_.deferRelease(
                [session = &session_, buffer, view, slot]() noexcept
                {
                    if (!slot.isNull())
                    {
                        session->descriptors().release(nri::DescriptorType::STRUCTURED_BUFFER, slot);
                    }
                    session->nriCore().DestroyDescriptor(view);
                    session->nriCore().DestroyBuffer(buffer);
                }
            );
        }
    }

    template <class Input>
    exec::task<RenderResult<RasterMaterialStorage::WriteValue<Input>>>
    RasterMaterialStorage::write(ReplicaMutation& mutation, Input input) noexcept
    {
        constexpr bool creating = std::same_as<Input, MaterialCommands::CreateInstance::Request>;
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        if (stop.stop_requested())
        {
            co_return cxx::unexpected(RenderError{kUploadStopped});
        }
        const bool has_nonfinite_parameter = std::ranges::any_of(
            input.parameters,
            [](const auto& parameter) noexcept
            { return !std::ranges::all_of(parameter, [](float value) noexcept { return std::isfinite(value); }); }
        );
        if (has_nonfinite_parameter)
        {
            co_return cxx::unexpected(RenderError{kStorageContract});
        }
        Entry* existing = nullptr;
        ProgramSlot* program = nullptr;
        if constexpr (creating)
        {
            program = findProgram(input.program);
            if (!program) co_return cxx::unexpected(RenderError{kStorageIdentity});
        }
        else
        {
            existing = findEntry(input.material);
            if (!existing)
            {
                co_return cxx::unexpected(RenderError{kStorageIdentity});
            }
            program = programs_.find(extension_detail::ResourceIdentity::local(existing->program));
        }
        if (!program->table)
        {
            if (!input.parameters.empty() || !input.textures.empty())
                co_return cxx::unexpected(RenderError{kStorageContract});
            if constexpr (creating)
            {
                const auto id = entries_.emplace();
                entries_.find(id)->program = program->record.id;
                ++program->instances;
                co_return extension_detail::ResourceIdentity::make<MaterialId>(id_, id);
            }
            else
            {
                co_return Ack{};
            }
        }
        auto& table = *program->table;
        const bool invalid_counts = input.parameters.size() != table.parameter_offsets.size() ||
                                    input.textures.size() != table.texture_offsets.size();
        if (invalid_counts)
        {
            co_return cxx::unexpected(RenderError{kStorageContract});
        }
        Page* selected = existing ? existing->page : nullptr;
        if (!selected)
        {
            bool occupied_writer = false;
            for (auto& page : table.pages)
            {
                if (page->free_slots.empty())
                {
                    continue;
                }
                if (page->updating)
                {
                    occupied_writer = true;
                }
                else
                {
                    selected = page.get();
                    break;
                }
            }
            if (!selected && occupied_writer)
            {
                co_return cxx::unexpected(RenderError{kDeviceBusy});
            }
            if (!selected)
            {
                auto created = createPage(table);
                if (!created)
                {
                    co_return cxx::unexpected(created.error());
                }
                selected = *created;
            }
        }
        auto& page = *selected;
        if (page.updating)
        {
            co_return cxx::unexpected(RenderError{kDeviceBusy});
        }
        page.updating = true;
        ++preparation_revision_;
        cxx::scope_exit finish_page(
            [&]() noexcept
            {
                page.updating = false;
                if (page.free_slots.size() == page.capacity)
                {
                    releasePage(page);
                    std::erase_if(table.pages, [&](const auto& item) noexcept { return item.get() == &page; });
                }
            }
        );
        const auto slot = existing ? existing->slot : page.free_slots.back();
        auto value_bytes = std::move(page.value_bytes);
        cxx::scope_exit recycle_values([&]() noexcept { page.value_bytes = std::move(value_bytes); });
        value_bytes.assign(table.stride, std::byte{});
        const auto values = std::span(value_bytes);
        for (std::size_t p = 0; p < input.parameters.size(); ++p)
        {
            std::memcpy(
                values.data() + table.parameter_offsets[p],
                input.parameters[p].data(),
                sizeof(input.parameters[p])
            );
        }
        for (std::size_t t = 0; t < input.textures.size(); ++t)
        {
            const auto texture = resolveTexture(input.textures[t]);
            if (!texture)
            {
                co_return cxx::unexpected(texture.error());
            }
            const auto& gpu = texture->reference;
            const rdesc::MaterialTextureGpuRef reference{0, gpu.image_index, gpu.sampler_index, gpu.flags};
            std::memcpy(values.data() + table.texture_offsets[t], &reference, sizeof(reference));
        }
        {
            auto replica = co_await mutation.select();
            if (!replica) co_return cxx::unexpected(replica.error());
            const auto frame_slot = replica->slot;
            auto& slice = page.gpu[frame_slot];
            cxx::scope_exit restore([&]() noexcept { markDirty(page, frame_slot, slot); });
            const BufferWrite write{slice.parameters, std::uint64_t(slot) * table.stride, values};
            if (auto uploaded = co_await mutation.writeBuffers({&write, 1}); !uploaded)
            {
                co_return cxx::unexpected(uploaded.error());
            }
            if (stop.stop_requested())
            {
                co_return cxx::unexpected(RenderError{kUploadStopped});
            }
            std::memcpy(page.records.data() + std::size_t(slot) * table.stride, values.data(), values.size());
            for (std::uint32_t frame = 0; frame < frame_count_; ++frame)
            {
                if (frame != frame_slot)
                {
                    markDirty(page, frame, slot);
                }
            }
            slice.dirty.acknowledge({slot / 64, std::uint64_t{1} << (slot % 64)});
            restore.release();
        }
        MaterialId id;
        if constexpr (creating)
        {
            id = extension_detail::ResourceIdentity::make<MaterialId>(id_, entries_.emplace());
            existing = entries_.find(extension_detail::ResourceIdentity::local(id));
            existing->program = program->record.id;
            existing->page = &page;
            existing->slot = slot;
            ++program->instances;
            page.free_slots.pop_back();
        }
        existing->textures = std::move(input.textures);
        if constexpr (creating)
        {
            co_return id;
        }
        else
        {
            co_return Ack{};
        }
    }

    exec::task<RenderResult<MaterialId>>
    RasterMaterialStorage::createInstance(
        ReplicaMutation& mutation,
        MaterialCommands::CreateInstance::Request input
    ) noexcept
    {
        return write(mutation, std::move(input));
    }

    exec::task<RenderResult<Ack>>
    RasterMaterialStorage::upload(ReplicaMutation& mutation, MaterialUpdate input) noexcept
    {
        return write(mutation, std::move(input));
    }

    RenderResult<void> RasterMaterialStorage::updateColors(std::span<const MaterialColorUpdate> updates) noexcept
    {
        if (updates.empty()) return {};
        if (preparation_revision_ == UINT64_MAX) return cxx::unexpected(RenderError{kStorageCapacity});
        cxx::scope_exit clear_selection([&]() noexcept { color_destinations_.clear(); });
        color_destinations_.reserve(updates.size());
        for (const auto& update : updates)
        {
            auto* entry = findEntry(update.material);
            if (!entry) return cxx::unexpected(RenderError{kStorageIdentity});
            auto* page = entry->page;
            const bool has_parameter = page && update.parameter < page->definition.parameter_offsets.size();
            const bool is_finite = std::ranges::all_of(update.linear_rgba, [](float value) noexcept
            {
                return std::isfinite(value);
            });
            if (!has_parameter || !is_finite) return cxx::unexpected(RenderError{kStorageContract});
            if (page->updating) return cxx::unexpected(RenderError{kDeviceBusy});
            const auto field = page->definition.parameter_offsets[update.parameter];
            const auto offset = std::size_t(entry->slot) * page->definition.stride + field;
            color_destinations_.push_back({page, offset, entry->slot});
        }
        for (std::size_t i = 0; i < updates.size(); ++i)
        {
            const auto& destination = color_destinations_[i];
            const auto& color = updates[i].linear_rgba;
            std::memcpy(destination.page->records.data() + destination.offset, color.data(), sizeof(color));
            for (std::uint32_t frame = 0; frame < frame_count_; ++frame)
            {
                markDirty(*destination.page, frame, destination.record);
            }
        }
        ++preparation_revision_;
        return {};
    }

    const RasterMaterialStorage::Entry* RasterMaterialStorage::findEntry(MaterialId id) const noexcept
    {
        return extension_detail::ResourceIdentity::parent(id) == id_
                   ? entries_.find(extension_detail::ResourceIdentity::local(id))
                   : nullptr;
    }

    RasterMaterialStorage::Entry* RasterMaterialStorage::findEntry(MaterialId id) noexcept
    {
        return const_cast<Entry*>(std::as_const(*this).findEntry(id));
    }

    RenderResult<rdesc::MaterialGpuReference>
    RasterMaterialStorage::gpuReference(MaterialId id, std::uint32_t frame) const noexcept
    {
        const auto* entry = findEntry(id);
        if (!entry)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        if (frame >= frame_count_)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        if (!entry->page) return rdesc::MaterialGpuReference{};
        return rdesc::MaterialGpuReference{entry->page->gpu[frame].slot.index, entry->slot};
    }

    MaterialProgramId RasterMaterialStorage::programId(MaterialId id) const noexcept
    {
        const auto* entry = findEntry(id);
        return entry ? entry->program : MaterialProgramId{};
    }

    RenderResult<RasterMaterialStorage::RasterRecord>
    RasterMaterialStorage::selectRasterRecord(
        MaterialId id, std::uint32_t frame, const rdesc::ShaderBlock& layout
    ) const noexcept
    {
        const auto* entry = findEntry(id);
        if (!entry) return cxx::unexpected(RenderError{kStorageIdentity});
        if (frame >= frame_count_) return cxx::unexpected(RenderError{kDeviceArgument});
        const auto* page = entry->page;
        if (!page || page->definition.layout != layout)
            return cxx::unexpected(RenderError{kStorageMismatch});
        const auto& slice = page->gpu[frame];
        const auto stride = page->definition.stride;
        return RasterRecord{
            {slice.slot.index, entry->slot}, {slice.parameters, std::uint64_t(entry->slot) * stride, stride},
            entry->textures
        };
    }

    RenderResult<const rdesc::MaterialProgramSet*>
    RasterMaterialStorage::beginProgramPreparation(MaterialProgramId id) noexcept
    {
        auto* slot = findProgram(id);
        if (!slot) return cxx::unexpected(RenderError{kStorageIdentity});
        if (slot->program_preparations == UINT32_MAX) return cxx::unexpected(RenderError{kStorageCapacity});
        ++slot->program_preparations;
        return &slot->record.data;
    }

    void RasterMaterialStorage::finishProgramPreparation(MaterialProgramId id) noexcept
    {
        auto* slot = findProgram(id);
        if (!slot || !slot->program_preparations) std::terminate();
        --slot->program_preparations;
    }

    const RasterMaterialStorage::ProgramSlot* RasterMaterialStorage::findProgram(MaterialProgramId id) const noexcept
    {
        if (extension_detail::ResourceIdentity::parent(id) != id_) return nullptr;
        return programs_.find(extension_detail::ResourceIdentity::local(id));
    }

    RasterMaterialStorage::ProgramSlot* RasterMaterialStorage::findProgram(MaterialProgramId id) noexcept
    {
        return const_cast<ProgramSlot*>(std::as_const(*this).findProgram(id));
    }

    RenderResult<void> RasterMaterialStorage::release(MaterialId id) noexcept
    {

        auto* entry = findEntry(id);
        if (!entry)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        auto* page = entry->page;
        if (page && page->updating)
        {
            return cxx::unexpected(RenderError{kDeviceBusy});
        }
        auto& program = *programs_.find(extension_detail::ResourceIdentity::local(entry->program));
        StorageSubscription::disconnect(entry->bindings);
        if (page)
        {
            page->free_slots.push_back(entry->slot);
            ++preparation_revision_;
        }
        entries_.erase(extension_detail::ResourceIdentity::local(id));
        --program.instances;
        if (page && page->free_slots.size() == page->capacity)
        {
            auto& table = page->definition;
            releasePage(*page);
            std::erase_if(table.pages, [&](const auto& value) noexcept { return value.get() == page; });
        }
        return {};
    }

    RenderResult<void>
    RasterMaterialStorage::resolve(
        const VGraphResourceDesc& plan,
        const MaterialBinding& argument,
        StorageSubscription& subscription,
        std::span<ResourceInput> output
    ) noexcept
    {
        auto* entry = findEntry(argument.material);
        if (!entry)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        if (!entry->page) return cxx::unexpected(RenderError{kStorageMismatch});
        if (output.size() > frame_count_)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        const auto& page = *entry->page;
        const auto& table = page.definition;
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
        if (argument.layout != table.layout)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        const auto* description = std::get_if<GraphBufferDesc>(&plan);
        if (!description || description->size < table.stride || description->size > page.records.size())
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        subscription.attach(entry->bindings);
        for (std::uint32_t frame = 0; frame < output.size(); ++frame)
        {
            output[frame] = {page.gpu[frame].parameters, 0, page.records.size()};
        }
        return {};
    }

    RenderResult<bool>
    RasterMaterialStorage::prepareFrame(
        std::uint32_t frame,
        ResourceUploader& uploads
    ) noexcept
    {
        auto& tables = dirty_pages_[frame];
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
            slice.accepted.clear();
            RenderResult<void> staged;
            const auto word_limit =
                (uploads.windowByteLimit() / table.definition.stride + 63) / 64 + uploads.windowRegionLimit();
            slice.dirty.select(word_limit, slice.selected);
            DirtyRanges::visitRanges(
                slice.selected,
                UINT32_MAX,
                [&](std::uint32_t first, std::uint32_t count) noexcept
                {
                    const auto begin = std::size_t(first) * table.definition.stride;
                    const auto source = std::span<const std::byte>(table.records)
                                            .subspan(begin, std::size_t(count) * table.definition.stride);
                    auto copied = uploads.stageBuffer({slice.parameters, begin, source});
                    if (!copied)
                    {
                        staged = cxx::unexpected(copied.error());
                        return false;
                    }
                    slice.dirty.detachRange(slice.accepted, first, count);
                    return true;
                }
            );
            if (!staged)
            {
                return cxx::unexpected(staged.error());
            }
            if (!slice.dirty.empty())
            {
                return cxx::unexpected(RenderError{kDeviceCapacity});
            }
        }
        return true;
    }

    void RasterMaterialStorage::finishFramePreparation(const ReplicaSettlement& settlement) noexcept
    {
        if (!settlement.copies.count) return;
        const auto frame = settlement.frame.slot;
        const bool accepted = !settlement.error.type;
        auto& dirty = dirty_pages_[frame];
        for (std::size_t index = 0; index < dirty.size();)
        {
            auto& table = *dirty[index];
            auto& slice = table.gpu[frame];
            slice.dirty.finishSelection(slice.accepted, bool(accepted));
            if (accepted && slice.dirty.empty())
            {
                removeDirty(table, frame);
            }
            else
            {
                ++index;
            }
        }
    }

    ResourceRegistration RasterMaterialStorage::factory() noexcept
    {

        static const auto frame_call =
            FrameDataCall::declare<MaterialResource, RasterMaterialStorage, &applyMaterialColors>();
        static const std::array commands{
            CommandBinding::declare<MaterialCommands::RegisterProgram, &handleRegisterProgram>(),
            CommandBinding::declare<MaterialCommands::CreateInstance, &handleCreateInstance>(),
            CommandBinding::declare<MaterialCommands::Update, &updateMaterial>(),
            CommandBinding::declare<MaterialCommands::UpdateColor, &updateMaterialColors>(),
            CommandBinding::declare<MaterialCommands::ReleaseInstance, &releaseInstance>(),
            CommandBinding::declare<MaterialCommands::UnregisterProgram, &handleUnregisterProgram>()
        };
        static const StorageBindingOps binding{
            [](void* object,
               const VGraphResourceDesc& plan,
               const void* argument,
               StorageSubscription& subscription,
               std::span<ResourceInput> output) noexcept
            {
                return static_cast<RasterMaterialStorage*>(object)
                    ->resolve(plan, *static_cast<const MaterialBinding*>(argument), subscription, output);
            },
            [](void* value, ReplicaFrame frame, ResourceUploader& uploads) noexcept
            { return static_cast<RasterMaterialStorage*>(value)->prepareFrame(frame.slot, uploads); },
            [](const void* value) noexcept
            { return &static_cast<const RasterMaterialStorage*>(value)->preparation_revision_; },
            nullptr, nullptr, nullptr,
            [](void* value, const ReplicaSettlement& settlement) noexcept
            { static_cast<RasterMaterialStorage*>(value)->finishFramePreparation(settlement); }
        };
        static constexpr auto create = +[](ResourceCreateContext& dependencies, ResourceInstanceId id
                                    ) noexcept -> RenderResult<std::unique_ptr<RasterMaterialStorage>>
        {
            if (dependencies.frame_count == 0 || dependencies.frame_count > 3)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            return std::make_unique<RasterMaterialStorage>(dependencies, id);
        };
        return makeResourceRegistration<render::MaterialResource, RasterMaterialStorage, create>(
            nullptr,
            &binding,
            commands,
            frame_call
        );
    }
} // namespace lux::render::detail
