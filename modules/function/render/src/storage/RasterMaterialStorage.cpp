#include <algorithm>
#include <bit>
#include <cstring>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/graph/RenderPlan.hpp>
#include <lux/engine/render/memory/Cleanup.hpp>
#include <lux/engine/render/memory/DirtyRanges.hpp>
#include <lux/engine/render/resource/StorageSet.hpp>
#include <lux/engine/render/storage/RasterMaterialStorage.hpp>
#include <lux/engine/render/storage/SampledTexture2DStorage.hpp>
#include <map>

namespace lux::render::detail
{
    struct RasterMaterialStorage::Table final
    {
        rdesc::MaterialProgramSet program;
        std::uint32_t stride = 0;
        std::vector<std::uint32_t> parameter_offsets, texture_offsets;
        std::vector<std::unique_ptr<Page>> pages;
        std::vector<ImageUse> image_uses;
        std::map<const nri::Texture*, std::uint32_t, std::less<>> image_indices;
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
            std::uint32_t partial_record = UINT32_MAX, next_partial_record = UINT32_MAX;
            std::size_t partial_bytes = 0, next_partial_bytes = 0;
        };

        std::array<GpuSlice, 3> gpu;
        std::vector<std::byte> value_bytes;
        bool updating = false;
    };

    RasterMaterialStorage::RasterMaterialStorage(StorageCreateContext& dependencies, StorageId id) noexcept
        : session_(dependencies.device), uploads_(*dependencies.uploads), frame_execution_(*dependencies.frames),
          resources_(dependencies.resources), id_(id)
    {
    }

    RasterMaterialStorage::~RasterMaterialStorage() noexcept
    {
        for (auto& entry : entries_)
        {
            StorageSubscription::disconnect(entry.bindings);
        }
        for (auto& table : tables_)
        {
            for (auto& page : table->pages) releasePage(*page);
        }
    }

    std::size_t RasterMaterialStorage::recordCount() const noexcept
    {
        return entries_.size();
    }

    RenderResult<RasterMaterialStorage::Table*>
    RasterMaterialStorage::table(rdesc::MaterialProgramSet& program) noexcept
    {
        for (auto& current : tables_)
        {
            const auto& existing = current->program;
            const bool same_layout = existing.parameters == program.parameters &&
                existing.parameter_defaults.size() == program.parameter_defaults.size() &&
                existing.texture_count == program.texture_count;
            const bool same_shader = existing.programs == program.programs &&
                existing.shading_model == program.shading_model && existing.alpha_mode == program.alpha_mode &&
                existing.double_sided == program.double_sided;
            if (same_layout && same_shader)
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
                            const bool invalid_binding = !layout || binding.block != *layout || binding.count != 0 ||
                                binding.type != rdesc::EDescriptorType::STORAGE_BUFFER ||
                                binding.writable || !binding.readable;
                            if (binding.name == rdesc::kMaterialTableName && invalid_binding)
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

    RasterMaterialStorage::TextureInput RasterMaterialStorage::resolveTexture(TextureId id) const noexcept
    {
        auto& registry = resources_;
        const auto* storage_set = registry.storageSet(extension_detail::ResourceIdentity::storage(id));
        if (!storage_set || (storage_set != registry.storageSet(id_) && !storage_set->isGlobal()))
        {
            return {};
        }
        auto source = registry.borrow<SampledTexture2DStorage>(extension_detail::ResourceIdentity::storage(id));
        const auto* texture = source ? (*source)->find(id) : nullptr;
        return texture ? TextureInput{texture->gpuReference(), &texture->image()} : TextureInput{};
    }

    void RasterMaterialStorage::addImageUses(Table& table, std::span<const nri::Texture* const> images) noexcept
    {
        for (auto* image : images)
        {
            const auto [position, inserted] =
                table.image_indices.emplace(image, static_cast<std::uint32_t>(table.image_uses.size()));
            if (inserted)
            {
                table.image_uses.push_back({image, 1});
            }
            else
            {
                ++table.image_uses[position->second].records;
            }
        }
    }

    void RasterMaterialStorage::removeImageUses(Table& table, std::span<const nri::Texture* const> images) noexcept
    {
        for (auto* image : images)
        {
            const auto position = table.image_indices.find(image);
            const auto index = position->second;
            if (--table.image_uses[index].records != 0)
            {
                continue;
            }
            const auto last = table.image_uses.back();
            table.image_uses[index] = last;
            table.image_indices[last.image] = index;
            table.image_uses.pop_back();
            table.image_indices.erase(position);
        }
    }

    RenderResult<RasterMaterialStorage::Page*> RasterMaterialStorage::createPage(Table& table) noexcept
    {
        const auto maximum = session_.description().memory.storageBufferMaxRange;
        if (table.stride > maximum) return cxx::unexpected(RenderError{kStorageCapacity});
        const auto page_bytes = std::min<std::uint64_t>(4u * 1024u * 1024u, maximum);
        const auto max_records = std::max<std::uint64_t>(1, std::bit_floor(page_bytes / table.stride));
        const auto requested = table.pages.empty() ? 16u : std::uint64_t(table.pages.back()->capacity) * 2;
        const auto capacity = static_cast<std::uint32_t>(std::min(requested, max_records));
        auto page = std::unique_ptr<Page>(new Page{table, capacity});
        page->records.resize(std::size_t(capacity) * table.stride);
        page->free_slots.reserve(capacity);
        for (auto i = capacity; i != 0; --i) page->free_slots.push_back(i - 1);
        cxx::scope_exit rollback([&]() noexcept { releasePage(*page); });
        const auto& core = session_.nriCore();
        for (std::uint32_t frame = 0; frame < frame_execution_.frameCount(); ++frame)
        {
            auto& slice = page->gpu[frame];
            slice.dirty.grow(capacity);
            // Constant-only programs have no material-buffer consumer or GPU page.
            if (!table.program.parameterLayout()) continue;
            const nri::BufferDesc description{
                page->records.size(), table.stride, nri::BufferUsageBits::SHADER_RESOURCE_STORAGE
            };
            auto status = core.CreatePlacedBuffer(session_.device(), NriDeviceHeap, description, slice.parameters);
            if (status != nri::Result::SUCCESS) return cxx::unexpected(nativeError(status));
            const nri::BufferViewDesc view{
                slice.parameters, nri::BufferView::STORAGE_STRUCTURED_BUFFER, 0, page->records.size()
            };
            status = core.CreateBufferView(view, slice.descriptor);
            if (status != nri::Result::SUCCESS) return cxx::unexpected(nativeError(status));
            auto slot = session_.descriptors().allocate(nri::DescriptorType::STORAGE_STRUCTURED_BUFFER);
            if (!slot) return cxx::unexpected(slot.error());
            slice.slot = *slot;
            session_.descriptors().write(nri::DescriptorType::STORAGE_STRUCTURED_BUFFER, *slot, slice.descriptor);
        }
        auto* result = page.get();
        rollback.release();
        table.pages.push_back(std::move(page));
        return result;
    }

    void RasterMaterialStorage::markDirty(Page& page, std::uint32_t frame, std::uint32_t record) noexcept
    {
        auto& slice = page.gpu[frame];
        if (!slice.parameters) return;
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
        if (position == UINT32_MAX) return;
        auto& list = dirty_pages_[frame];
        list[position] = list.back();
        list[position]->gpu[frame].position = position;
        list.pop_back();
        position = UINT32_MAX;
    }

    void RasterMaterialStorage::releasePage(Page& page) noexcept
    {
        for (std::uint32_t frame = 0; frame < frame_execution_.frameCount(); ++frame)
        {
            removeDirty(page, frame);
            auto& slice = page.gpu[frame];
            auto* buffer = std::exchange(slice.parameters, nullptr);
            auto* view = std::exchange(slice.descriptor, nullptr);
            auto slot = std::exchange(slice.slot, {});
            if (!buffer && !view && slot.isNull()) continue;
            session_.deferRelease([session = &session_, buffer, view, slot]() noexcept
            {
                if (!slot.isNull())
                {
                    session->descriptors().release(nri::DescriptorType::STORAGE_STRUCTURED_BUFFER, slot);
                }
                session->nriCore().DestroyDescriptor(view);
                session->nriCore().DestroyBuffer(buffer);
            });
        }
    }

    template <class Input>
    exec::task<RenderResult<RasterMaterialStorage::WriteValue<Input>>>
    RasterMaterialStorage::write(Input input) noexcept
    {
        Entry* existing = nullptr;
        Table* definition = nullptr;
        if constexpr (std::same_as<Input, MaterialData>)
        {
            auto found = table(input.program);
            if (!found) co_return cxx::unexpected(found.error());
            definition = *found;
        }
        else
        {
            existing = extension_detail::ResourceIdentity::storage(input.material) == id_
                ? entries_.find(extension_detail::ResourceIdentity::local(input.material)) : nullptr;
            if (!existing) co_return cxx::unexpected(RenderError{kStorageIdentity});
            definition = &existing->page->definition;
        }
        auto& table = *definition;
        cxx::scope_exit remove_empty_definition([&]() noexcept
        {
            if (table.pages.empty())
                std::erase_if(tables_, [&](const auto& item) noexcept { return item.get() == &table; });
        });
        const bool invalid_counts = input.parameters.size() != table.parameter_offsets.size() ||
            input.textures.size() != table.texture_offsets.size();
        if (invalid_counts) co_return cxx::unexpected(RenderError{kStorageContract});
        Page* selected = existing ? existing->page : nullptr;
        if (!selected)
        {
            bool occupied_writer = false;
            for (auto& page : table.pages)
            {
                if (page->free_slots.empty()) continue;
                if (page->updating) occupied_writer = true;
                else { selected = page.get(); break; }
            }
            if (!selected && occupied_writer) co_return cxx::unexpected(RenderError{kDeviceBusy});
            if (!selected)
            {
                auto created = createPage(table);
                if (!created) co_return cxx::unexpected(created.error());
                selected = *created;
            }
        }
        auto& page = *selected;
        if (page.updating) co_return cxx::unexpected(RenderError{kDeviceBusy});
        page.updating = true;
        ++preparation_revision_;
        cxx::scope_exit finish_page([&]() noexcept
        {
            page.updating = false;
            if (page.free_slots.size() == page.capacity)
            {
                releasePage(page);
                std::erase_if(table.pages, [&](const auto& item) noexcept { return item.get() == &page; });
            }
        });
        const auto slot = existing ? existing->slot : page.free_slots.back();
        page.value_bytes.assign(table.stride, std::byte{});
        const auto values = std::span(page.value_bytes);
        for (std::size_t p = 0; p < input.parameters.size(); ++p)
            std::memcpy(
                values.data() + table.parameter_offsets[p], input.parameters[p].data(), sizeof(input.parameters[p])
            );
        std::vector<const nri::Texture*> images;
        images.reserve(input.textures.size());
        for (std::size_t t = 0; t < input.textures.size(); ++t)
        {
            const auto texture = resolveTexture(input.textures[t]);
            const auto& gpu = texture.reference;
            const MaterialTextureGpuRef reference{0, gpu.image_index, gpu.sampler_index, gpu.flags};
            if (texture.image) images.push_back(texture.image);
            std::memcpy(values.data() + table.texture_offsets[t], &reference, sizeof(reference));
        }
        std::uint32_t frame_slot = 0;
        SubmissionFrontiers progress;
        bool write_acquired = false;
        cxx::scope_exit finish_frame([&]() noexcept
        {
            if (write_acquired) frame_execution_.finishWrite(frame_slot, progress);
        });
        if (table.program.parameterLayout())
        {
            while (!frame_execution_.tryBeginWrite(frame_slot))
            {
                if (auto ready = co_await frame_execution_.whenWritable(values.size()); !ready)
                    co_return cxx::unexpected(ready.error());
            }
            write_acquired = true;
            auto& slice = page.gpu[frame_slot];
            cxx::scope_exit restore([&]() noexcept { markDirty(page, frame_slot, slot); });
            const BufferWrite write{slice.parameters, std::uint64_t(slot) * table.stride, values};
            if (auto uploaded = co_await uploads_.writeBuffers({&write, 1}, {}, frame_slot, &progress); !uploaded)
                co_return cxx::unexpected(uploaded.error());
            std::memcpy(page.records.data() + std::size_t(slot) * table.stride, values.data(), values.size());
            for (std::uint32_t frame = 0; frame < frame_execution_.frameCount(); ++frame)
                if (frame != frame_slot) markDirty(page, frame, slot);
            slice.dirty.acknowledge({slot / 64, std::uint64_t{1} << (slot % 64)});
            if (slice.partial_record == slot)
            {
                slice.partial_record = UINT32_MAX;
                slice.partial_bytes = 0;
            }
            restore.release();
        }
        MaterialId id;
        if constexpr (std::same_as<Input, MaterialData>)
        {
            id = extension_detail::ResourceIdentity::make<MaterialId>(id_, entries_.emplace());
            existing = entries_.find(extension_detail::ResourceIdentity::local(id));
            existing->page = &page;
            existing->slot = slot;
            page.free_slots.pop_back();
        }
        if (existing->images != images)
        {
            removeImageUses(table, existing->images);
            addImageUses(table, images);
            existing->images = std::move(images);
        }
        if constexpr (std::same_as<Input, MaterialData>) co_return id;
        else co_return Ack{};
    }

    exec::task<RenderResult<MaterialId>> RasterMaterialStorage::upload(MaterialData input) noexcept
    {
        return write(std::move(input));
    }

    exec::task<RenderResult<Ack>> RasterMaterialStorage::upload(MaterialUpdate input) noexcept
    {
        return write(std::move(input));
    }

    RenderResult<rdesc::MaterialGpuReference>
    RasterMaterialStorage::gpuReference(MaterialId id, std::uint32_t frame) const noexcept
    {
        const auto* entry = extension_detail::ResourceIdentity::storage(id) == id_
            ? entries_.find(extension_detail::ResourceIdentity::local(id)) : nullptr;
        if (!entry) return cxx::unexpected(RenderError{kStorageIdentity});
        return rdesc::MaterialGpuReference{entry->page->gpu[frame].slot.index, entry->slot};
    }

    const rdesc::MaterialProgramSet* RasterMaterialStorage::program(MaterialId id) const noexcept
    {
        const auto* entry = extension_detail::ResourceIdentity::storage(id) == id_
            ? entries_.find(extension_detail::ResourceIdentity::local(id)) : nullptr;
        return entry ? &entry->page->definition.program : nullptr;
    }

    RenderResult<void>
    RasterMaterialStorage::collectReads(
        MaterialId source,
        std::uint32_t frame,
        std::vector<ResourceInput>& buffers,
        std::vector<const nri::Texture*>& images
    ) const noexcept
    {
        const auto* entry = extension_detail::ResourceIdentity::storage(source) == id_
            ? entries_.find(extension_detail::ResourceIdentity::local(source)) : nullptr;
        if (!entry) return cxx::unexpected(RenderError{kStorageIdentity});
        for (const auto& page : entry->page->definition.pages)
        {
            if (page->free_slots.size() == page->capacity) continue;
            if (auto* buffer = page->gpu[frame].parameters) buffers.push_back({buffer, 0, page->records.size()});
        }
        for (const auto& use : entry->page->definition.image_uses) images.push_back(use.image);
        return {};
    }

    RenderResult<void> RasterMaterialStorage::release(MaterialId id) noexcept
    {
        if (frame_execution_.submitting())
        {
            return cxx::unexpected(RenderError{kDeviceBusy});
        }
        auto* entry = extension_detail::ResourceIdentity::storage(id) == id_
                          ? entries_.find(extension_detail::ResourceIdentity::local(id))
                          : nullptr;
        if (!entry)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        if (entry->page->updating)
        {
            return cxx::unexpected(RenderError{kDeviceBusy});
        }
        auto& page = *entry->page;
        auto& table = page.definition;
        StorageSubscription::disconnect(entry->bindings);
        removeImageUses(table, entry->images);
        page.free_slots.push_back(entry->slot);
        ++preparation_revision_;
        entries_.erase(extension_detail::ResourceIdentity::local(id));
        if (page.free_slots.size() == page.capacity)
        {
            releasePage(page);
            std::erase_if(table.pages, [&](const auto& value) noexcept { return value.get() == &page; });
            if (table.pages.empty())
                std::erase_if(tables_, [&](const auto& value) noexcept { return value.get() == &table; });
        }
        return {};
    }

    RenderResult<void> RasterMaterialStorage::resolve(
        const VGraphResourceDesc& plan,
        const MaterialBinding& argument,
        StorageSubscription& subscription,
        std::span<ResourceInput> output
    ) noexcept
    {
        auto* entry = extension_detail::ResourceIdentity::storage(argument.material) == id_
                          ? entries_.find(extension_detail::ResourceIdentity::local(argument.material))
                          : nullptr;
        if (!entry)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
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
        const auto* layout = table.program.parameterLayout();
        if (!layout || argument.layout != *layout)
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

    RenderResult<bool> RasterMaterialStorage::prepareFrame(std::uint32_t frame, ResourceUploader& uploads) noexcept
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
                            auto& storage = *static_cast<RasterMaterialStorage*>(value);
                            storage.finishFramePreparation(frame, accepted);
                        }
                    );
                }
            }
        );
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
            slice.next_partial_record = slice.partial_record;
            slice.next_partial_bytes = slice.partial_bytes;
            std::uint64_t covered = 0;
            bool progress = false;
            const auto stage = [&](std::uint32_t first, std::uint32_t count, std::size_t offset
                               ) noexcept -> RenderResult<bool>
            {
                while (count)
                {
                    const auto begin = std::size_t(first) * table.definition.stride + offset;
                    const auto source = std::span<const std::byte>(table.records)
                                            .subspan(begin, std::size_t(count) * table.definition.stride - offset);
                    auto* target = slice.parameters;
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
                    const auto records = static_cast<std::uint32_t>(total / table.definition.stride);
                    if (records)
                    {
                        DirtyRanges::appendRange(slice.accepted, first, records);
                        covered += records;
                    }
                    first += records;
                    count -= records;
                    offset = total % table.definition.stride;
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
                    (uploads.windowByteLimit() / table.definition.stride + 63) / 64 + uploads.windowRegionLimit();
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
            ready &= covered == slice.dirty.recordCount();
        }
        return ready;
    }

    void RasterMaterialStorage::finishFramePreparation(std::uint32_t frame, RenderResult<void> accepted) noexcept
    {
        auto& dirty = dirty_pages_[frame];
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
                removeDirty(table, frame);
            }
            else
            {
                ++index;
            }
        }
    }

    StorageFactory RasterMaterialStorage::factory() noexcept
    {
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
            [](void* object, std::uint32_t slot, ResourceUploader& uploads) noexcept
            { return static_cast<RasterMaterialStorage*>(object)->prepareFrame(slot, uploads); },
            [](const void* value) noexcept
            { return &static_cast<const RasterMaterialStorage*>(value)->preparation_revision_; }
        };
        static const auto create = +[](StorageCreateContext& dependencies, StorageId id
                                    ) noexcept -> RenderResult<std::unique_ptr<RasterMaterialStorage>>
        {
            if (!dependencies.uploads || !dependencies.frames)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            return std::make_unique<RasterMaterialStorage>(dependencies, id);
        };
        return makeStorageFactory<
            render::RasterMaterialStorageDescriptor,
            RasterMaterialStorage,
            StorageCreateContext,
            create>(nullptr, &binding);
    }
} // namespace lux::render::detail

namespace lux::render
{
    exec::task<RenderResult<MaterialId>> StorageTraits<detail::RasterMaterialStorage>::upload(
        detail::RasterMaterialStorage& storage,
        MaterialData input
    ) noexcept
    {
        return storage.upload(std::move(input));
    }

    exec::task<RenderResult<Ack>> StorageTraits<detail::RasterMaterialStorage>::upload(
        detail::RasterMaterialStorage& storage,
        MaterialUpdate input
    ) noexcept
    {
        return storage.upload(std::move(input));
    }
} // namespace lux::render
