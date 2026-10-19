#include "Cleanup.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/material/MaterialStorage.hpp>

namespace lux::render::vulkan
{
    struct MaterialStorage::Table final
    {
        MaterialProgram program;
        std::uint32_t stride = 0;
        std::vector<std::uint32_t> parameter_offsets, texture_offsets;
        std::vector<std::byte> records; // The sole CPU parameter authority, in the reflected GPU layout.
        std::vector<Entry*> entries;
        std::vector<std::uint32_t> free_slots;
        gapi::vk::VmaBuffer buffer;
        bool updating = false;
        std::vector<std::uint32_t> texture_changes;
        std::vector<bool> texture_changed;
    };

    namespace
    {
        void beforeWrite(VkCommandBuffer command) noexcept
        {
            VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(command, &dependency);
        }

        void afterWrite(VkCommandBuffer command) noexcept
        {
            VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(command, &dependency);
        }
    } // namespace

    MaterialStorage::MaterialStorage(DeviceSession& session, StorageId id) noexcept : session_(session), id_(id) {}

    MaterialStorage::~MaterialStorage() noexcept
    {
        observers_.clear();
        for (auto& entry : entries_)
        {
            StorageSubscription::disconnect(entry.bindings);
        }
        for (auto& table : tables_)
        {
            releaseAfterGpu(session_, table->buffer);
        }
    }

    std::size_t MaterialStorage::recordCount() const noexcept
    {
        std::size_t count = 0;
        for (const auto& entry : entries_)
        {
            count += entry.revision != 0;
        }
        return count;
    }

    RenderResult<MaterialStorage::Table*> MaterialStorage::table(MaterialProgram& program) noexcept
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
        const bool is_invalid_shader = program.fragment.stage != rdesc::EShaderType::FRAGMENT ||
                                       program.fragment.entry.empty() || program.fragment.words.size() < 5 ||
                                       program.fragment.words.front() != 0x07230203;
        if (is_invalid_layout || is_invalid_shader)
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        auto candidate = std::make_unique<Table>();
        candidate->stride = layout->size;
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
        for (std::size_t i = 0; i < program.parameter_count; ++i)
        {
            const auto* parameter = field("params[" + std::to_string(i) + "]", rdesc::EDataScalar::FLOAT, 4);
            if (!parameter)
            {
                return cxx::unexpected(RenderError{kStorageContract});
            }
            candidate->parameter_offsets.push_back(parameter->offset);
        }
        constexpr std::array<std::string_view, 4> members{"representation_index", "resource_index", "aux", "flags"};
        for (std::uint32_t i = 0; i < program.texture_count; ++i)
        {
            std::uint32_t base = 0;
            for (std::uint32_t m = 0; m < members.size(); ++m)
            {
                const auto* member =
                    field("tex[" + std::to_string(i) + "]." + std::string(members[m]), rdesc::EDataScalar::UINT, 1);
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

    MaterialStorage::TextureKey MaterialStorage::key(TextureId id) noexcept
    {
        return {id.storage.index, id.storage.gen, id.local.index, id.local.gen};
    }

    std::uint32_t MaterialStorage::textureIndex(TextureId id) const noexcept
    {
        auto& registry = session_.resources();
        const auto* owner = registry.owner(id.storage);
        if (!owner || (owner != registry.owner(id_) && owner != &session_.globalStorages()))
        {
            return 0;
        }
        auto source = registry.borrow<TextureStorage>(id.storage);
        const auto* texture = source ? (*source)->find(id) : nullptr;
        return texture ? texture->descriptorIndex() : 0;
    }

    void MaterialStorage::watch(TextureId id) noexcept
    {
        const StorageKey storage{id.storage.index, id.storage.gen};
        if (observers_.contains(storage))
        {
            return;
        }
        auto& registry = session_.resources();
        const auto* owner = registry.owner(id.storage);
        if (!owner || (owner != registry.owner(id_) && owner != &session_.globalStorages()))
        {
            return;
        }
        auto source = registry.borrow<TextureStorage>(id.storage);
        if (!source)
        {
            return; // Stale Texture identity remains a logical white reference.
        }
        auto& observer = observers_.try_emplace(storage).first->second;
        observer.receiver = this;
        observer.affected = [](const void* value, TextureId id) noexcept
        { return static_cast<const MaterialStorage*>(value)->references_.contains(key(id)); };
        observer.record = [](void* value, VkCommandBuffer command, TextureId id, std::uint32_t descriptor) noexcept
        { static_cast<MaterialStorage*>(value)->recordTexture(command, id, descriptor); };
        observer.adopt = [](void* value, TextureId id, std::uint32_t descriptor) noexcept
        { static_cast<MaterialStorage*>(value)->adoptTexture(id, descriptor); };
        (*source)->observe(observer);
    }

    void MaterialStorage::link(MaterialId id, Entry& entry, std::span<const TextureId> textures) noexcept
    {
        entry.textures.reserve(textures.size());
        for (std::uint32_t slot = 0; slot < textures.size(); ++slot)
        {
            const auto texture = textures[slot];
            auto& references = references_[key(texture)];
            entry.textures.push_back({texture, references.size()});
            references.emplace_back(id, slot);
            watch(texture);
        }
    }

    void MaterialStorage::unlink(Entry& entry) noexcept
    {
        for (auto& texture : entry.textures)
        {
            auto found = references_.find(key(texture.texture));
            auto& references = found->second;
            references[texture.position] = references.back();
            const auto [id, slot] = references[texture.position];
            entries_.find(id.local)->textures[slot].position = texture.position;
            references.pop_back();
            if (references.empty())
            {
                references_.erase(found);
            }
        }
        entry.textures.clear();
    }

    void MaterialStorage::recordTexture(VkCommandBuffer command, TextureId id, std::uint32_t descriptor) noexcept
    {
        const auto found = references_.find(key(id));
        if (found == references_.end())
        {
            return;
        }
        for (const auto [material, texture] : found->second)
        {
            const auto& entry = *entries_.find(material.local);
            const auto& table = *entry.table;
            const auto offset = std::uint64_t(entry.slot) * table.stride + table.texture_offsets[texture] +
                                offsetof(MaterialTextureGpuRef, resource_index);
            vkCmdUpdateBuffer(command, table.buffer.handle(), offset, sizeof(descriptor), &descriptor);
        }
    }

    void MaterialStorage::adoptTexture(TextureId id, std::uint32_t descriptor) noexcept
    {
        const auto found = references_.find(key(id));
        if (found == references_.end())
        {
            return;
        }
        for (const auto [material, texture] : found->second)
        {
            const auto& entry = *entries_.find(material.local);
            auto& table = *entry.table;
            const auto offset = std::size_t(entry.slot) * table.stride + table.texture_offsets[texture] +
                                offsetof(MaterialTextureGpuRef, resource_index);
            std::memcpy(table.records.data() + offset, &descriptor, sizeof(descriptor));
            if (table.updating && !table.texture_changed[entry.slot])
            {
                table.texture_changed[entry.slot] = true;
                table.texture_changes.push_back(entry.slot);
            }
        }
    }

    void MaterialStorage::publish(Table& table) noexcept
    {
        for (auto* entry : table.entries)
        {
            if (entry)
            {
                StorageSubscription::publish(entry->bindings, {table.buffer.handle(), 0, table.buffer.size()});
            }
        }
    }

    MaterialStorage::UploadResult MaterialStorage::upload(MaterialWrite input) noexcept
    {
        if (input.material.storage != id_)
        {
            co_return cxx::unexpected(RenderError{kStorageIdentity});
        }
        auto* existing = input.material.isNull() ? nullptr : entries_.find(input.material.local);
        if (!input.material.isNull() && (!existing || !existing->revision))
        {
            co_return cxx::unexpected(RenderError{kStorageIdentity});
        }
        const auto revision = existing ? existing->revision : 0;
        if (input.expected_revision != revision || revision == UINT64_MAX)
        {
            co_return cxx::unexpected(RenderError{kStorageContract});
        }
        if (existing && existing->updating)
        {
            co_return cxx::unexpected(RenderError{kBusy});
        }
        const bool invalid_counts = input.parameters.size() != input.program.parameter_count ||
                                    input.textures.size() != input.program.texture_count;
        const bool invalid_values = std::ranges::any_of(
            input.parameters,
            [](const auto& lane)
            { return !std::ranges::all_of(lane, [](float value) { return std::isfinite(value); }); }
        );
        if (invalid_counts || invalid_values)
        {
            co_return cxx::unexpected(RenderError{kStorageContract});
        }
        // A Material identity belongs to one cooked table ABI. Changing the program creates a new
        // Material, so existing instance GPU slots cannot silently change interpretation.
        if (existing && existing->table->program != input.program)
        {
            co_return cxx::unexpected(RenderError{kStorageMismatch});
        }
        auto selected = table(input.program);
        if (!selected)
        {
            co_return cxx::unexpected(selected.error());
        }
        auto& table = **selected;
        if (table.updating)
        {
            co_return cxx::unexpected(RenderError{kBusy});
        }
        const auto slot = existing                    ? existing->slot
                          : !table.free_slots.empty() ? table.free_slots.back()
                                                      : static_cast<std::uint32_t>(table.entries.size());
        const auto current_capacity = table.records.size() / table.stride;
        const bool growing = slot >= current_capacity;
        const auto capacity =
            growing ? std::max<std::uint64_t>(16, std::bit_ceil(std::uint64_t(slot) + 1)) : current_capacity;
        const auto total_bytes = capacity * table.stride;
        if (total_bytes > session_.physicalDevice().properties().properties.limits.maxStorageBufferRange ||
            table.entries.size() >= UINT32_MAX)
        {
            co_return cxx::unexpected(RenderError{kStorageCapacity});
        }
        std::vector<std::byte> values(table.stride);
        for (std::size_t p = 0; p < input.parameters.size(); ++p)
        {
            std::memcpy(
                values.data() + table.parameter_offsets[p],
                input.parameters[p].data(),
                sizeof(input.parameters[p])
            );
        }
        const auto setTextures = [&]() noexcept
        {
            for (std::size_t t = 0; t < input.textures.size(); ++t)
            {
                const MaterialTextureGpuRef reference{0, textureIndex(input.textures[t]), 0, 0};
                std::memcpy(values.data() + table.texture_offsets[t], &reference, sizeof(reference));
            }
        };
        setTextures();
        auto id = input.material;
        if (id.isNull())
        {
            id.local = entries_.emplace();
        }
        auto& entry = *entries_.find(id.local);
        entry.updating = table.updating = true;
        gapi::vk::VmaBuffer candidate;
        cxx::scope_exit rollback(
            [&]() noexcept
            {
                entry.updating = table.updating = false;
                for (auto changed : table.texture_changes)
                {
                    table.texture_changed[changed] = false;
                }
                table.texture_changes.clear();
                if (!entry.revision)
                {
                    entries_.erase(id.local);
                }
                releaseAfterGpu(session_, candidate);
                if (table.entries.empty())
                {
                    const auto empty =
                        std::ranges::find_if(tables_, [&](const auto& value) { return value.get() == &table; });
                    tables_.erase(empty);
                }
            }
        );
        std::vector<std::byte> grown;
        if (growing)
        {
            grown = table.records;
            grown.resize(total_bytes);
            std::memcpy(grown.data() + std::size_t(slot) * table.stride, values.data(), values.size());
        }
        const auto source = growing ? std::span<const std::byte>(grown) : std::span<const std::byte>(values);
        auto allocated = session_.allocator().createBuffer(
            source.size(),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            EMemoryAccess::DEVICE
        );
        if (!allocated)
        {
            co_return cxx::unexpected(allocated.error());
        }
        candidate = std::move(*allocated);
        auto uploaded = co_await session_.uploads().writeBuffer(candidate, source);
        if (!uploaded)
        {
            co_return cxx::unexpected(uploaded.error());
        }
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        std::optional<CommandBatch> batch;
        for (;;)
        {
            if (stop.stop_requested())
            {
                co_return cxx::unexpected(RenderError{kWorkStopped});
            }
            auto ready = session_.queue().begin();
            if (ready)
            {
                batch.emplace(std::move(*ready));
                break;
            }
            if (ready.error().type != kCapacity)
            {
                co_return cxx::unexpected(ready.error());
            }
            auto completed = co_await session_.queue().whenAvailable();
            if (!completed)
            {
                co_return cxx::unexpected(completed.error());
            }
        }
        const auto command = batch->native();
        auto& target = growing ? candidate : table.buffer;
        beforeWrite(command);
        if (!growing)
        {
            const VkBufferCopy copy{0, std::uint64_t(slot) * table.stride, table.stride};
            vkCmdCopyBuffer(command, candidate.handle(), target.handle(), 1, &copy);
            beforeWrite(command);
        }
        // Texture publications may have completed during CPU preparation. Apply their actual latest
        // fields to the candidate and re-resolve this request's logical references before the single commit.
        if (growing)
        {
            for (auto changed : table.texture_changes)
            {
                for (auto offset : table.texture_offsets)
                {
                    const auto at = std::size_t(changed) * table.stride + offset;
                    std::memcpy(grown.data() + at, table.records.data() + at, sizeof(MaterialTextureGpuRef));
                    vkCmdUpdateBuffer(command, target.handle(), at, sizeof(MaterialTextureGpuRef), grown.data() + at);
                }
            }
        }
        setTextures();
        for (auto offset : table.texture_offsets)
        {
            vkCmdUpdateBuffer(
                command,
                target.handle(),
                std::uint64_t(slot) * table.stride + offset,
                sizeof(MaterialTextureGpuRef),
                values.data() + offset
            );
        }
        afterWrite(command);
        auto accepted = std::move(*batch).submit();
        if (!accepted)
        {
            co_return cxx::unexpected(accepted.error());
        }
        if (growing)
        {
            releaseAfterGpu(session_, table.buffer);
            table.buffer = std::move(candidate);
            table.records = std::move(grown);
            table.texture_changed.resize(capacity, false);
        }
        std::memcpy(table.records.data() + std::size_t(slot) * table.stride, values.data(), values.size());
        if (entry.revision)
        {
            unlink(entry);
        }
        else if (!table.free_slots.empty())
        {
            table.free_slots.pop_back();
        }
        if (slot == table.entries.size())
        {
            table.entries.push_back(&entry);
        }
        else
        {
            table.entries[slot] = &entry;
        }
        entry.table = &table;
        entry.slot = slot;
        entry.revision = revision + 1;
        link(id, entry, input.textures);
        if (growing)
        {
            publish(table);
        }
        co_return MaterialPublished{id, entry.revision}; // cleanup releases only the temporary staging candidate.
    }

    RenderResult<std::uint32_t> MaterialStorage::gpuIndex(MaterialId id) const noexcept
    {
        const auto* entry = id.storage == id_ ? entries_.find(id.local) : nullptr;
        if (!entry || !entry->revision)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        return entry->slot;
    }

    bool MaterialStorage::sameTable(MaterialId left, MaterialId right) const noexcept
    {
        const auto* a = left.storage == id_ ? entries_.find(left.local) : nullptr;
        const auto* b = right.storage == id_ ? entries_.find(right.local) : nullptr;
        return a && b && a->revision && b->revision && a->table == b->table;
    }

    RenderResult<MaterialInformation> MaterialStorage::query(MaterialId id) const noexcept
    {
        const auto* entry = id.storage == id_ ? entries_.find(id.local) : nullptr;
        if (!entry || !entry->revision)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        const auto& table = *entry->table;
        MaterialInformation result{{id, entry->revision}, table.program};
        result.parameters.resize(table.parameter_offsets.size());
        const auto* bytes = table.records.data() + std::size_t(entry->slot) * table.stride;
        for (std::size_t p = 0; p < result.parameters.size(); ++p)
        {
            std::memcpy(result.parameters[p].data(), bytes + table.parameter_offsets[p], sizeof(result.parameters[p]));
        }
        for (const auto& link : entry->textures)
        {
            result.textures.push_back(link.texture);
        }
        return result;
    }

    RenderResult<void> MaterialStorage::release(MaterialId id, std::uint64_t revision) noexcept
    {
        auto* entry = id.storage == id_ ? entries_.find(id.local) : nullptr;
        if (!entry || !entry->revision)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        if (entry->updating || entry->table->updating)
        {
            return cxx::unexpected(RenderError{kBusy});
        }
        if (entry->revision != revision)
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        auto& table = *entry->table;
        StorageSubscription::disconnect(entry->bindings);
        unlink(*entry);
        table.entries[entry->slot] = nullptr;
        table.free_slots.push_back(entry->slot);
        entries_.erase(id.local);
        if (table.free_slots.size() == table.entries.size())
        {
            releaseAfterGpu(session_, table.buffer);
            const auto empty = std::ranges::find_if(tables_, [&](const auto& value) { return value.get() == &table; });
            tables_.erase(empty);
        }
        return {};
    }

    RenderResult<StorageResource> MaterialStorage::resolve(
        const NativeResourcePlan& plan,
        const MaterialBinding& argument,
        StorageSubscription& subscription
    ) noexcept
    {
        auto* entry = argument.material.storage == id_ ? entries_.find(argument.material.local) : nullptr;
        if (!entry || !entry->revision)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        const auto& table = *entry->table;
        if (argument.program != table.program)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        if (argument.texture_slot != UINT32_MAX)
        {
            const auto* description = std::get_if<GraphImageDesc>(&plan.description);
            if (!description || argument.texture_slot >= entry->textures.size())
            {
                return cxx::unexpected(RenderError{kStorageMismatch});
            }
            const auto texture = entry->textures[argument.texture_slot].texture;
            auto storage = session_.resources().borrow<TextureStorage>(texture.storage);
            const auto* current = storage ? (*storage)->find(texture) : nullptr;
            if (current)
            {
                const auto& actual = current->info();
                const auto format = actual.pixel_format == rdesc::ETexturePixelFormat::RGBA8_UNORM
                                        ? rdesc::ETextureFormat::RGBA8_UNORM
                                    : actual.pixel_format == rdesc::ETexturePixelFormat::RGBA8_SRGB
                                        ? rdesc::ETextureFormat::RGBA8_SRGB
                                        : rdesc::ETextureFormat::BC1_RGBA_SRGB;
                const bool is_mismatch = description->width != actual.width || description->height != actual.height ||
                                         description->mips != actual.mip_count ||
                                         description->layers != actual.layers || description->format != format;
                if (is_mismatch)
                {
                    return cxx::unexpected(RenderError{kStorageMismatch});
                }
            }
            // The shared descriptor table is immutable in shape. Material owns the actual integer
            // reference updates; a Context has no Texture subscription or professional write to perform.
            return StorageResource{};
        }
        const auto* description = std::get_if<GraphBufferDesc>(&plan.description);
        if (!description || description->size < table.stride || description->size > table.buffer.size())
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        subscription.attach(entry->bindings);
        return StorageResource{table.buffer.handle(), 0, table.buffer.size()};
    }

    StorageFactory materialStorageFactory() noexcept
    {
        static const render::MaterialStorage::Ops ops{
            [](const void* object, MaterialId id) noexcept
            { return static_cast<const MaterialStorage*>(object)->query(id); },
            [](void* object, MaterialId id, std::uint64_t revision) noexcept
            { return static_cast<MaterialStorage*>(object)->release(id, revision); }
        };
        static const StorageBindingOps binding{
            [](void* object, const NativeResourcePlan& plan, const void* argument, StorageSubscription& subscription
            ) noexcept
            {
                return static_cast<MaterialStorage*>(object)
                    ->resolve(plan, *static_cast<const MaterialBinding*>(argument), subscription);
            }
        };
        static const auto create =
            +[](DeviceSession& session, StorageId id) noexcept -> RenderResult<std::unique_ptr<MaterialStorage>>
        { return std::make_unique<MaterialStorage>(session, id); };
        return makeStorageFactory<render::MaterialStorage, MaterialStorage, DeviceSession, create>(ops, &binding);
    }
} // namespace lux::render::vulkan

namespace lux::render
{
    StorageTraits<vulkan::MaterialStorage>::Result StorageTraits<vulkan::MaterialStorage>::upload(
        vulkan::MaterialStorage& storage,
        MaterialWrite input
    ) noexcept
    {
        return storage.upload(std::move(input));
    }
} // namespace lux::render
