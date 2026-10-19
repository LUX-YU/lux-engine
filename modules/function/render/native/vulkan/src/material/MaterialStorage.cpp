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
        return entries_.size();
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

    template <class Input>
    exec::task<RenderResult<MaterialStorage::WriteValue<Input>>> MaterialStorage::write(Input input) noexcept
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
            co_return cxx::unexpected(RenderError{kBusy});
        }
        table.updating = true;
        gapi::vk::VmaBuffer candidate;
        cxx::scope_exit rollback(
            [&]() noexcept
            {
                table.updating = false;
                releaseAfterGpu(session_, candidate);
                if (table.entries.empty())
                {
                    const auto empty =
                        std::ranges::find_if(tables_, [&](const auto& value) { return value.get() == &table; });
                    tables_.erase(empty);
                }
            }
        );
        const bool invalid_counts = input.parameters.size() != table.parameter_offsets.size() ||
                                    input.textures.size() != table.texture_offsets.size();
        const bool invalid_values = std::ranges::any_of(
            input.parameters,
            [](const auto& lane)
            { return !std::ranges::all_of(lane, [](float value) { return std::isfinite(value); }); }
        );
        if (invalid_counts || invalid_values)
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
        // Resolve only the explicit request. Client retains these references through completion and
        // updates Materials before releasing Textures; no cross-Storage observer repairs later misuse.
        for (std::size_t t = 0; t < input.textures.size(); ++t)
        {
            const MaterialTextureGpuRef reference{0, textureIndex(input.textures[t]), 0, 0};
            std::memcpy(values.data() + table.texture_offsets[t], &reference, sizeof(reference));
        }
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
        beforeWrite(command);
        if (!growing)
        {
            const VkBufferCopy copy{0, std::uint64_t(slot) * table.stride, table.stride};
            vkCmdCopyBuffer(command, candidate.handle(), table.buffer.handle(), 1, &copy);
        }
        afterWrite(command);
        if (auto accepted = std::move(*batch).submit(); !accepted)
        {
            co_return cxx::unexpected(accepted.error());
        }
        if (growing)
        {
            releaseAfterGpu(session_, table.buffer);
            table.buffer = std::move(candidate);
            table.records = std::move(grown);
        }
        std::memcpy(table.records.data() + std::size_t(slot) * table.stride, values.data(), values.size());
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
        existing->textures = std::move(input.textures);
        if (growing)
        {
            publish(table);
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

    exec::task<RenderResult<MaterialId>> MaterialStorage::upload(MaterialWrite input) noexcept
    {
        return write(std::move(input));
    }

    exec::task<RenderResult<Ack>> MaterialStorage::upload(MaterialUpdate input) noexcept
    {
        return write(std::move(input));
    }

    RenderResult<std::uint32_t> MaterialStorage::gpuIndex(MaterialId id) const noexcept
    {
        const auto* entry = id.storage == id_ ? entries_.find(id.local) : nullptr;
        if (!entry)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        return entry->slot;
    }

    bool MaterialStorage::sameTable(MaterialId left, MaterialId right) const noexcept
    {
        const auto* a = left.storage == id_ ? entries_.find(left.local) : nullptr;
        const auto* b = right.storage == id_ ? entries_.find(right.local) : nullptr;
        return a && b && a->table == b->table;
    }

    RenderResult<void> MaterialStorage::release(MaterialId id) noexcept
    {
        auto* entry = id.storage == id_ ? entries_.find(id.local) : nullptr;
        if (!entry)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        if (entry->table->updating)
        {
            return cxx::unexpected(RenderError{kBusy});
        }
        auto& table = *entry->table;
        StorageSubscription::disconnect(entry->bindings);
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
        if (!entry)
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
            const auto texture = entry->textures[argument.texture_slot];
            auto storage = session_.resources().borrow<TextureStorage>(texture.storage);
            const auto* current = storage ? (*storage)->find(texture) : nullptr;
            if (current)
            {
                const auto& actual = current->image();
                const auto format = actual.format() == VK_FORMAT_R8G8B8A8_UNORM  ? rdesc::ETextureFormat::RGBA8_UNORM
                                    : actual.format() == VK_FORMAT_R8G8B8A8_SRGB ? rdesc::ETextureFormat::RGBA8_SRGB
                                                                                 : rdesc::ETextureFormat::BC1_RGBA_SRGB;
                const bool is_mismatch = description->width != actual.extent().width ||
                                         description->height != actual.extent().height ||
                                         description->mips != actual.mipLevels() ||
                                         description->layers != actual.arrayLayers() || description->format != format;
                if (is_mismatch)
                {
                    return cxx::unexpected(RenderError{kStorageMismatch});
                }
            }
            // The descriptor table is immutable in shape. Only explicit Material updates change the
            // stored integer references; Context does not repair Client resource dependencies.
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
        return makeStorageFactory<render::MaterialStorage, MaterialStorage, DeviceSession, create>(nullptr, &binding);
    }
} // namespace lux::render::vulkan

namespace lux::render
{
    exec::task<RenderResult<MaterialId>> StorageTraits<vulkan::MaterialStorage>::upload(
        vulkan::MaterialStorage& storage,
        MaterialWrite input
    ) noexcept
    {
        return storage.upload(std::move(input));
    }

    exec::task<RenderResult<Ack>> StorageTraits<vulkan::MaterialStorage>::upload(
        vulkan::MaterialStorage& storage,
        MaterialUpdate input
    ) noexcept
    {
        return storage.upload(std::move(input));
    }
} // namespace lux::render
