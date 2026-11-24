#include <lux/engine/render/resource/material/Resource.hpp>
#include <lux/engine/render/effect/EffectFrameWriter.hpp>
#include <lux/engine/render/graph/execute/RasterDraws.hpp>
#include <algorithm>
#include <ranges>
#include <bit>
#include <cmath>
#include <cstring>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/graph/GraphResource.hpp>
#include <lux/engine/render/memory/Cleanup.hpp>
#include <lux/engine/render/memory/DirtyRanges.hpp>
#include <lux/engine/render/resource/ResourceSet.hpp>
#include <lux/engine/render/resource/material/MaterialStorage.hpp>
#include <lux/engine/render/effect/implementations/mesh_common/IndexedDraws.hpp>
#include <lux/engine/render/effect/implementations/mesh_common/PreparedMeshPipelines.hpp>
#include <lux/engine/render/runtime/DeviceEntry.hpp>
#include <lux/engine/render/resource/texture/SampledTexture2DStorage.hpp>
#include <utility>

namespace lux::render::detail
{
    struct MaterialStorage::Table final
    {
        rdesc::ShaderBlock layout;
        std::array<std::optional<rdesc::EDescriptorBindingInfo>, 3> bindings;
        std::size_t digest = 0;
        std::size_t programs = 0;
        std::vector<std::uint32_t> parameter_offsets, texture_offsets;
        std::vector<std::unique_ptr<Page>> pages;
    };

    struct MaterialStorage::Page final
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
    };

    MaterialStorage::MaterialStorage(ResourceCreateContext& dependencies, ResourceInstanceId id) noexcept
        : device_(dependencies.device_owner_), has_uploads_(dependencies.uploads != nullptr), frame_count_(dependencies.frame_count),
          resources_(dependencies.resources), id_(id)
    {
    }

    MaterialStorage::~MaterialStorage() noexcept
    {
        for (auto& [digest, table] : tables_)
        {
            for (auto& page : table->pages)
            {
                releasePage(*page);
            }
        }
    }

    std::size_t MaterialStorage::recordCount() const noexcept
    {
        return entries_.size();
    }

    RenderResult<MaterialStorage::RegisteredProgram>
    MaterialStorage::registerProgram(rdesc::MaterialProgramSet program) noexcept
    {
        if (program.target != device_.session().enabledFeatures().shader_target)
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
            const bool has_record_support = device_.session().resourceFeatures().containsAny(EGpuResourceFeature::STORAGE_BUFFER_ARRAY);
            if (!has_record_support) return cxx::unexpected(RenderError{kDeviceUnsupported});
            if (!has_uploads_) return cxx::unexpected(RenderError{kDeviceArgument});
        }
        std::array<bool, 3> purposes{};
        RecordBindings bindings{};
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
                    bindings[purpose] = &binding;
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
            auto resolved = qualifyTable(program, *layout, bindings);
            if (!resolved) return cxx::unexpected(resolved.error());
            table = *resolved;
        }
        const auto key = programs_.emplace();
        RegisteredProgram result;
        result.program = extension_detail::ResourceIdentity::make<MaterialProgramId>(id_, key);
        auto* slot = programs_.find(key);
        const bool paged = device_.session().usesDeviceAddresses();
        for (auto& use : program.programs)
        {
            const auto purpose = static_cast<std::size_t>(use.purpose);
            slot->mesh[purpose].shader = prepareMeshShader(program, std::move(use), paged);
            result.indexed_mesh_shader[purpose] = slot->mesh[purpose].shader != nullptr;
        }
        slot->table = table;
        if (table) ++table->programs;
        return result;
    }

    exec::task<RenderResult<void>>
    MaterialStorage::prepareMeshPipelines(MaterialMeshPipelines request) noexcept
    {
        auto* slot = findProgram(request.program);
        if (!slot) co_return cxx::unexpected(RenderError{kStorageIdentity});
        const auto purpose = static_cast<std::size_t>(request.purpose);
        if (purpose >= slot->mesh.size()) co_return cxx::unexpected(RenderError{kStorageContract});
        if (slot->mesh[purpose].preparing) co_return cxx::unexpected(RenderError{kDeviceBusy});
        auto& publication = slot->mesh[purpose].prepared;
        if (request.uses.empty())
        {
            if (publication)
            {
                invalidateSurfacePrograms(request.program, request.purpose);
                publication.reset();
            }
            co_return RenderResult<void>{};
        }
        if (!slot->mesh[purpose].shader) co_return cxx::unexpected(RenderError{kGraphInvalidDescriptor});

        // All mesh and material reads finish before the worker can suspend. The admitted resource command keeps
        // this program registered; independent purposes and the previously published pipelines remain available.
        if (publication)
        {
            const auto current = publication->matches(request, resources_);
            if (!current) co_return cxx::unexpected(current.error());
            if (*current) co_return RenderResult<void>{};
        }
        slot->mesh[purpose].preparing = true;
        cxx::scope_exit finish([&]() noexcept { slot->mesh[purpose].preparing = false; });
        auto candidate = co_await PreparedMeshPipelines::create(device_, request, *slot->mesh[purpose].shader, resources_);
        if (!candidate) co_return cxx::unexpected(candidate.error());
        invalidateSurfacePrograms(request.program, request.purpose);
        publication = std::move(*candidate);
        co_return RenderResult<void>{};
    }

    RenderResult<MaterialStorage::Table*> MaterialStorage::qualifyTable(
        const rdesc::MaterialProgramSet& program, const rdesc::ShaderBlock& layout, const RecordBindings& bindings
    ) noexcept
    {
        const auto digest = (std::size_t(layout.size) * 0x9e3779b9u) ^ layout.alignment ^ layout.fields.size();
        const auto [first, last] = tables_.equal_range(digest);
        for (auto current = first; current != last; ++current)
        {
            auto& table = *current->second;
            // For the same reflected block and field counts, the named projections below are identical.
            const bool same_projection = table.parameter_offsets.size() == program.parameter_defaults.size() &&
                table.texture_offsets.size() == program.texture_count && table.layout == layout;
            if (!same_projection) continue;
            const bool same_bindings = std::ranges::equal(table.bindings, bindings, [](const auto& owned, const auto* input)
            {
                return input ? owned && *owned == *input : !owned;
            });
            if (same_bindings) return &table;
        }

        auto candidate = std::make_unique<Table>();
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
        for (std::size_t i = 0; i < bindings.size(); ++i)
            if (bindings[i]) candidate->bindings[i] = *bindings[i];
        auto* result = candidate.get();
        candidate->digest = digest;
        tables_.emplace(digest, std::move(candidate));
        return result;
    }

    RenderResult<void> MaterialStorage::unregisterProgram(MaterialProgramId id) noexcept
    {
        auto* program = findProgram(id);
        if (!program) return cxx::unexpected(RenderError{kStorageIdentity});
        if (program->instances) return cxx::unexpected(RenderError{kDeviceBusy});
        for (std::size_t purpose = 0; purpose < program->mesh.size(); ++purpose)
        {
            if (!program->mesh[purpose].prepared) continue;
            invalidateSurfacePrograms(id, static_cast<rdesc::EMaterialPurpose>(purpose));
        }
        auto* table = program->table;
        programs_.erase(extension_detail::ResourceIdentity::local(id));
        if (table)
        {
            --table->programs;
            removeEmptyTable(*table);
        }
        return {};
    }

    void MaterialStorage::removeEmptyTable(Table& table) noexcept
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

    RenderResult<TextureGpuRef> MaterialStorage::resolveTexture(TextureId id) const noexcept
    {
        if (id.isNull())
        {
            return TextureGpuRef{};
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
        return texture->gpuReference();
    }

    RenderResult<MaterialStorage::Page*> MaterialStorage::createPage(Table& table) noexcept
    {
        const auto maximum = device_.session().description().memory.storageBufferMaxRange;
        if (table.layout.size > maximum)
        {
            return cxx::unexpected(RenderError{kStorageCapacity});
        }
        const auto page_bytes = std::min<std::uint64_t>(4u * 1024u * 1024u, maximum);
        const auto max_records = std::max<std::uint64_t>(1, std::bit_floor(page_bytes / table.layout.size));
        const auto requested = table.pages.empty() ? 16u : std::uint64_t(table.pages.back()->capacity) * 2;
        const auto capacity = static_cast<std::uint32_t>(std::min(requested, max_records));
        auto page = std::unique_ptr<Page>(new Page{table, capacity});
        page->records.resize(std::size_t(capacity) * table.layout.size);
        page->free_slots.reserve(capacity);
        for (auto i = capacity; i != 0; --i)
        {
            page->free_slots.push_back(i - 1);
        }
        cxx::scope_exit rollback([&]() noexcept { releasePage(*page); });
        const auto& core = device_.session().nriCore();
        for (std::uint32_t frame = 0; frame < frame_count_; ++frame)
        {
            auto& slice = page->gpu[frame];
            slice.dirty.grow(capacity);
            const nri::BufferDesc description{
                page->records.size(),
                table.layout.size,
                (nri::BufferUsageBits::SHADER_RESOURCE | nri::BufferUsageBits::SHADER_RESOURCE_STORAGE)
            };
            auto status = core.CreatePlacedBuffer(device_.session().device(), NriDeviceHeap, description, slice.parameters);
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
            auto slot = device_.session().descriptors().allocate(nri::DescriptorType::STRUCTURED_BUFFER);
            if (!slot)
            {
                return cxx::unexpected(slot.error());
            }
            slice.slot = *slot;
            device_.session().descriptors().write(nri::DescriptorType::STRUCTURED_BUFFER, *slot, slice.descriptor);
        }
        auto* result = page.get();
        rollback.release();
        table.pages.push_back(std::move(page));
        return result;
    }

    void MaterialStorage::markDirty(Page& page, std::uint32_t frame, std::uint32_t record) noexcept
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

    void MaterialStorage::removeDirty(Page& page, std::uint32_t frame) noexcept
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

    void MaterialStorage::releasePage(Page& page) noexcept
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
            device_.session().deferRelease(
                [session = &device_.session(), buffer, view, slot]() noexcept
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
    RenderResult<MaterialStorage::WriteValue<Input>> MaterialStorage::write(const Input& input) noexcept
    {
        constexpr bool creating = std::same_as<Input, MaterialInstanceData>;
        const bool has_nonfinite_parameter = std::ranges::any_of(
            input.parameters,
            [](const auto& parameter) noexcept
            { return !std::ranges::all_of(parameter, [](float value) noexcept { return std::isfinite(value); }); }
        );
        if (has_nonfinite_parameter)
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        Entry* existing = nullptr;
        ProgramSlot* program = nullptr;
        if constexpr (creating)
        {
            program = findProgram(input.program);
            if (!program) return cxx::unexpected(RenderError{kStorageIdentity});
        }
        else
        {
            existing = findEntry(input.material);
            if (!existing)
            {
                return cxx::unexpected(RenderError{kStorageIdentity});
            }
            program = programs_.find(extension_detail::ResourceIdentity::local(existing->program));
        }
        if (!program->table)
        {
            if (!input.parameters.empty() || !input.textures.empty())
                return cxx::unexpected(RenderError{kStorageContract});
            if constexpr (creating)
            {
                const auto id = entries_.emplace();
                entries_.find(id)->program = input.program;
                ++program->instances;
                return extension_detail::ResourceIdentity::make<MaterialId>(id_, id);
            }
            else
            {
                return {};
            }
        }
        auto& table = *program->table;
        const bool invalid_counts = input.parameters.size() != table.parameter_offsets.size() ||
                                    input.textures.size() != table.texture_offsets.size();
        if (invalid_counts)
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        // Qualify and pack the entire replacement before touching an installed record or allocating a new page.
        record_scratch_.assign(table.layout.size, std::byte{});
        auto values = std::span{record_scratch_};
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
                return cxx::unexpected(texture.error());
            }
            const rdesc::MaterialTextureGpuRef reference{0, texture->image_index, texture->sampler_index, texture->flags};
            std::memcpy(values.data() + table.texture_offsets[t], &reference, sizeof(reference));
        }
        Page* selected = existing ? existing->page : nullptr;
        if (!selected)
        {
            const auto available = std::ranges::find_if(table.pages, [](const auto& page) { return !page->free_slots.empty(); });
            if (available != table.pages.end()) selected = available->get();
            else
            {
                auto created = createPage(table);
                if (!created) return cxx::unexpected(created.error());
                selected = *created;
            }
        }
        auto& page = *selected;
        const auto slot = existing ? existing->slot : page.free_slots.back();
        std::memcpy(page.records.data() + std::size_t{slot} * table.layout.size, values.data(), values.size());
        for (std::uint32_t frame = 0; frame < frame_count_; ++frame) markDirty(page, frame, slot);
        MaterialId id;
        if constexpr (creating)
        {
            id = extension_detail::ResourceIdentity::make<MaterialId>(id_, entries_.emplace());
            existing = entries_.find(extension_detail::ResourceIdentity::local(id));
            existing->program = input.program;
            existing->page = &page;
            existing->slot = slot;
            ++program->instances;
            page.free_slots.pop_back();
        }
        auto& reads = existing->textures;
        reads.clear();
        for (auto texture : input.textures)
            if (!texture.isNull()) reads.push_back(texture);
        std::ranges::sort(reads, std::less<>{});
        reads.erase(std::ranges::unique(reads).begin(), reads.end());
        if constexpr (creating)
        {
            return id;
        }
        else
        {
            return {};
        }
    }

    RenderResult<MaterialId> MaterialStorage::createInstance(const MaterialInstanceData& input) noexcept
    {
        return write(input);
    }

    RenderResult<void> MaterialStorage::update(const MaterialUpdate& input) noexcept
    {
        return write(input);
    }

    RenderResult<void> MaterialStorage::updateColors(std::span<const MaterialColorUpdate> updates) noexcept
    {
        if (updates.empty()) return {};
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
            const auto field = page->definition.parameter_offsets[update.parameter];
            const auto offset = std::size_t(entry->slot) * page->definition.layout.size + field;
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
        return {};
    }

    const MaterialStorage::Entry* MaterialStorage::findEntry(MaterialId id) const noexcept
    {
        return extension_detail::ResourceIdentity::parent(id) == id_
                   ? entries_.find(extension_detail::ResourceIdentity::local(id))
                   : nullptr;
    }

    MaterialStorage::Entry* MaterialStorage::findEntry(MaterialId id) noexcept
    {
        return const_cast<Entry*>(std::as_const(*this).findEntry(id));
    }

    MaterialProgramId MaterialStorage::programOf(MaterialId id) const noexcept
    {
        const auto* entry = findEntry(id);
        return entry ? entry->program : MaterialProgramId{};
    }

    RenderResult<rdesc::MaterialGpuReference>
    MaterialStorage::gpuReference(MaterialId id, std::uint32_t frame) const noexcept
    {
        const auto* entry = findEntry(id);
        if (!entry) return cxx::unexpected(RenderError{kStorageIdentity});
        if (frame >= frame_count_) return cxx::unexpected(RenderError{kDeviceArgument});
        if (!entry->page) return rdesc::MaterialGpuReference{};
        return rdesc::MaterialGpuReference{entry->page->gpu[frame].slot.index, entry->slot};
    }

    RenderResult<MaterialStorage::RasterRecord>
    MaterialStorage::selectRasterRecord(MaterialId id, const RasterRecordLayout& layout) const noexcept
    {
        const auto* entry = findEntry(id);
        if (!entry) return cxx::unexpected(RenderError{kStorageIdentity});
        if (layout.frame_slot >= frame_count_) return cxx::unexpected(RenderError{kDeviceArgument});
        const auto* page = entry->page;
        if (!page || page->definition.layout != layout.block)
            return cxx::unexpected(RenderError{kStorageMismatch});
        const auto& slice = page->gpu[layout.frame_slot];
        const auto stride = page->definition.layout.size;
        return RasterRecord{
            {slice.slot.index, entry->slot}, slice.parameters, {std::uint64_t(entry->slot) * stride, stride},
            layout.reads_images ? std::span<const TextureId>{entry->textures} : std::span<const TextureId>{}
        };
    }

    RenderResult<rdesc::MaterialGpuReference> MaterialStorage::RasterRecord::record(
        CpuDrawStorage& output, RasterStreamRange programs, const SampledTexture2DStorage* images
    ) const noexcept
    {
        if (!textures.empty() && !images) return cxx::unexpected(RenderError{kStorageIdentity});
        const auto first = output.record_images.size();
        if constexpr (kCollectReadMetrics)
            if (textures.size() > kGraphInvalidIndex - first) return cxx::unexpected(RenderError{kDeviceCapacity});
        for (auto texture : textures)
        {
            const auto* image = images->find(texture);
            if (!image)
            {
                if constexpr (kCollectReadMetrics) output.record_images.resize(first);
                return cxx::unexpected(RenderError{kStorageIdentity});
            }
            if constexpr (kCollectReadMetrics)
            {
                const auto& desc = image->description();
                output.record_images.push_back({&image->image(), {1, 0, desc.mipNum, 0, desc.layerNum}});
            }
        }
        if constexpr (kCollectReadMetrics)
        {
            for (std::uint32_t i = 0; i < programs.count; ++i)
                output.records.push_back({
                    buffer, bytes, programs.first + i, static_cast<std::uint32_t>(first),
                    static_cast<std::uint32_t>(textures.size())
                });
        }
        return reference;
    }

    RenderResult<const PreparedMeshPipelines*>
    MaterialStorage::publishedMeshPipelines(MaterialProgramId id, rdesc::EMaterialPurpose purpose) const & noexcept
    {
        const auto* program = findProgram(id);
        if (!program) return cxx::unexpected(RenderError{kStorageIdentity});
        const auto index = static_cast<std::size_t>(purpose);
        if (index >= program->mesh.size() || !program->mesh[index].shader)
            return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
        return program->mesh[index].prepared.get();
    }

    const MaterialStorage::ProgramSlot* MaterialStorage::findProgram(MaterialProgramId id) const noexcept
    {
        if (extension_detail::ResourceIdentity::parent(id) != id_) return nullptr;
        return programs_.find(extension_detail::ResourceIdentity::local(id));
    }

    MaterialStorage::ProgramSlot* MaterialStorage::findProgram(MaterialProgramId id) noexcept
    {
        return const_cast<ProgramSlot*>(std::as_const(*this).findProgram(id));
    }

    RenderResult<void> MaterialStorage::release(MaterialId id) noexcept
    {

        auto* entry = findEntry(id);
        if (!entry)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        auto* page = entry->page;
        auto& program = *programs_.find(extension_detail::ResourceIdentity::local(entry->program));
        if (page) page->free_slots.push_back(entry->slot);
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

    RenderResult<bool>
    MaterialStorage::prepareFrame(
        std::uint32_t frame,
        ResourceUploader& uploads
    ) noexcept
    {
        bool ready = true;
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
                (uploads.windowByteLimit() / table.definition.layout.size + 63) / 64 + uploads.windowRegionLimit();
            slice.dirty.select(word_limit, slice.selected);
            DirtyRanges::visitRanges(
                slice.selected,
                UINT32_MAX,
                [&](std::uint32_t first, std::uint32_t count) noexcept
                {
                    const auto begin = std::size_t(first) * table.definition.layout.size;
                    const auto source = std::span<const std::byte>(table.records)
                                            .subspan(begin, std::size_t(count) * table.definition.layout.size);
                    const auto stride = table.definition.layout.size;
                    auto copied = uploads.stageBufferPrefix({slice.parameters, begin, source}, stride);
                    if (copied && !*copied)
                    {
                        // A large record may span several staging pages. Keep its dirty bit until
                        // the complete record can be attached to the same copy receipt.
                        if (stride > uploads.windowByteLimit())
                        {
                            staged = cxx::unexpected(RenderError{kDeviceCapacity});
                            return false;
                        }
                        copied = uploads.stageBuffer({slice.parameters, begin, source.first(stride)});
                        if (!copied && copied.error().type == kDeviceCapacity) return false;
                    }
                    if (!copied)
                    {
                        staged = cxx::unexpected(copied.error());
                        return false;
                    }
                    const auto records = static_cast<std::uint32_t>(*copied / stride);
                    slice.dirty.detachRange(slice.accepted, first, records);
                    return records == count;
                }
            );
            if (!staged)
            {
                return cxx::unexpected(staged.error());
            }
            ready &= slice.dirty.empty();
        }
        return ready;
    }

    void MaterialStorage::finishFramePreparation(const ReplicaSettlement& settlement) noexcept
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


    ResourceRegistration MaterialStorage::factory() noexcept
    {

        static const StorageBindingOps binding{
            .prepareFrame = [](void* value, ReplicaFrame frame, ResourceUploader& uploads) noexcept
            { return static_cast<MaterialStorage*>(value)->prepareFrame(frame.slot, uploads); },
            .settleFrame = [](void* value, const ReplicaSettlement& settlement) noexcept
            { static_cast<MaterialStorage*>(value)->finishFramePreparation(settlement); }
        };
        static constexpr auto create = +[](ResourceCreateContext& dependencies, ResourceInstanceId id
                                    ) noexcept -> RenderResult<std::unique_ptr<MaterialStorage>>
        {
            if (dependencies.frame_count == 0 || dependencies.frame_count > 3)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            return std::make_unique<MaterialStorage>(dependencies, id);
        };
        return makeResourceRegistration<render::MaterialResource, MaterialStorage, create, &MaterialStorage::updateColors>(&binding);
    }
} // namespace lux::render::detail

namespace lux::render
{
    RenderResult<MaterialDrawBinding> MaterialDrawBinding::bind(
        const InstalledDrawSources& sources, RasterResourceSlot records, std::optional<RasterResourceSlot> textures
    ) noexcept
    {
        const auto material = sources.bind<detail::MaterialStorage>(records, EDrawResourceRead::STORAGE_RECORDS);
        if (!material) return cxx::unexpected(material.error());
        if (!material->owner) return cxx::unexpected(RenderError{kStorageIdentity});
        const detail::SampledTexture2DStorage* images = nullptr;
        if (textures)
        {
            const auto selected = sources.bind<detail::SampledTexture2DStorage>(*textures, EDrawResourceRead::SAMPLED_IMAGES);
            if (!selected) return cxx::unexpected(selected.error());
            if (&selected->output != &material->output) return cxx::unexpected(RenderError{kStorageMismatch});
            images = selected->owner;
        }
        return MaterialDrawBinding{*material->owner, images, material->output};
    }

    RenderResult<rdesc::MaterialGpuReference>
    MaterialDrawBinding::record(RasterProgramSlot program, MaterialId id) const noexcept
    {
        const auto layout = output_.recordLayout(program);
        if (!layout) return cxx::unexpected(layout.error());
        const auto record = materials_.selectRasterRecord(id, *layout);
        if (!record) return cxx::unexpected(record.error());
        return record->record(output_, {program.index, 1}, textures_);
    }
} // namespace lux::render
