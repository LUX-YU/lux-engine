#include "BufferArena.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/StorageBinding.hpp>
#include <lux/engine/render/native/vulkan/material/MaterialStorage.hpp>
#include <lux/engine/render/native/vulkan/texture/TextureStorage.hpp>

namespace lux::render::vulkan
{
    struct MaterialStorage::Table final
    {
        MaterialProgram program;
        BufferArena arena;

        Table(DeviceSession& session, MaterialProgram value) noexcept
            : program(std::move(value)), arena(session, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 64 * 1024)
        {
        }
    };

    struct MaterialStorage::MaterialData final
    {
        MaterialData(MaterialProgram value, MaterialParameters params, std::vector<TextureId> references) noexcept
            : program(std::move(value)), parameters(std::move(params)), textures(std::move(references))
        {
            const auto& cooked = program;
            bytes = cooked.recordSize();
            record.resize(bytes);
            std::memcpy(record.data(), parameters.data(), parameters.size() * sizeof(parameters[0]));
            texture_info.resize(textures.size());
            texture_bindings.resize(textures.size());
        }

        void setTexture(std::size_t slot, std::uint32_t index) noexcept
        {
            const auto offset = 16 * std::max<std::size_t>(1, parameters.size()) + slot * 16 + 4;
            std::memcpy(record.data() + offset, &index, sizeof(index));
        }

        MaterialProgram program;
        MaterialParameters parameters;
        std::vector<TextureId> textures;
        std::vector<rdesc::TextureInfo> texture_info;
        std::vector<PreparedStorageBinding> texture_bindings;
        MaterialStorage* owner = nullptr;
        MaterialId id;
        std::vector<std::byte> record;
        std::unique_ptr<BufferArena::Range> range;
        RetirementReservation retirement;
        std::uint64_t bytes = 0, revision = 0;
    };

    struct MaterialStorage::Use final
    {
        MaterialStorage* storage;
        MaterialId id;
        PreparedStorageBinding& binding;
        std::size_t position;
        std::uint32_t texture_slot;

        void update(const std::unique_ptr<MaterialData>& value) noexcept
        {
            if (texture_slot == UINT32_MAX)
            {
                binding.buffer = value->range->buffer;
                binding.buffer_offset = value->range->offset;
                binding.pending_bytes = value->record;
            }
            binding.publishUpdate();
        }

        ~Use() noexcept
        {
            if (storage)
            {
                storage->unsubscribe(*this);
            }
        }
    };

    struct MaterialStorage::Candidate final
    {
        MaterialStorage& storage;
        MaterialId id;
        std::unique_ptr<MaterialData> backing;

        ~Candidate() noexcept
        {
            if (!backing)
            {
                return;
            }
            auto& entry = *storage.entries_.find(id.local);
            entry.updating = false;
            if (!entry.current)
            {
                storage.entries_.erase(id.local);
            }
            storage.retire(backing);
        }

        void publish() noexcept
        {
            auto& entry = *storage.entries_.find(id.local);
            storage.retire(entry.current);
            entry.current = std::move(backing);
            auto& data = *entry.current;
            data.owner = &storage;
            data.id = id;
            for (auto& texture : data.texture_bindings)
            {
                texture.update_context = &data;
                texture.updated = [](void* value) noexcept
                {
                    auto& data = *static_cast<MaterialData*>(value);
                    data.owner->refreshTextures(data);
                };
            }
            entry.updating = false;
            for (auto* use : entry.uses)
            {
                use->update(entry.current);
            }
        }
    };

    MaterialStorage::MaterialStorage(DeviceSession& session, StorageId id) noexcept : session_(session), id_(id) {}

    MaterialStorage::~MaterialStorage() noexcept
    {
        for (auto& entry : entries_)
        {
            disconnect(entry);
            retire(entry.current);
        }
    }

    std::size_t MaterialStorage::recordCount() const noexcept
    {
        return entries_.size();
    }

    MaterialStorage::UploadResult MaterialStorage::upload(MaterialWrite input) noexcept
    {
        if (input.material.storage != id_)
        {
            co_return cxx::unexpected(RenderError{kStorageIdentity});
        }
        auto* existing = input.material.isNull() ? nullptr : entries_.find(input.material.local);
        if (!input.material.isNull() && !existing)
        {
            co_return cxx::unexpected(RenderError{kStorageIdentity});
        }
        if (existing && existing->updating)
        {
            co_return cxx::unexpected(RenderError{kBusy});
        }
        const auto revision = existing ? existing->current->revision : 0;
        if (input.expected_revision != revision || revision == std::numeric_limits<std::uint64_t>::max())
        {
            co_return cxx::unexpected(RenderError{kStorageContract});
        }
        const auto* program = &input.program;
        const bool invalid_program =
            program->fragment.stage != rdesc::EShaderType::FRAGMENT || program->fragment.entry.empty() ||
            program->fragment.words.size() < 5 || program->fragment.words.front() != 0x07230203 ||
            input.parameters.size() != program->defaults.size() || input.textures.size() != program->texture_count;
        if (invalid_program)
        {
            co_return cxx::unexpected(RenderError{kStorageContract});
        }
        for (const auto& lane : input.parameters)
        {
            for (const auto value : lane)
            {
                if (!std::isfinite(value))
                {
                    co_return cxx::unexpected(RenderError{kStorageContract});
                }
            }
        }
        // Only the cold Graph owns a Pipeline. A bound Material cannot silently replace its Shader contract.
        if (existing && !existing->uses.empty() && existing->current->program != *program)
        {
            co_return cxx::unexpected(RenderError{kStorageMismatch});
        }
        const auto record_bytes = program->recordSize();
        if (record_bytes > session_.physicalDevice().properties().properties.limits.maxStorageBufferRange)
        {
            co_return cxx::unexpected(RenderError{kStorageCapacity});
        }
        auto backing =
            std::make_unique<MaterialData>(input.program, std::move(input.parameters), std::move(input.textures));
        for (std::uint32_t slot = 0; slot < program->texture_count; ++slot)
        {
            const auto texture = backing->textures[slot];
            auto& registry = session_.resources();
            auto* owner = registry.owner(texture.storage);
            const bool invalid_owner = owner != &session_.globalStorages() && owner != registry.owner(id_);
            if (!owner || invalid_owner)
            {
                continue; // Invalid logical Texture references sample the Session's white slot.
            }
            auto storage = registry.borrow<TextureStorage>(texture.storage);
            if (!storage)
            {
                continue;
            }
            auto reference = (*storage)->bind(texture, backing->texture_bindings[slot]);
            if (!reference)
            {
                co_return cxx::unexpected(reference.error());
            }
            backing->texture_info[slot] = reference->first;
            backing->setTexture(slot, reference->second);
        }
        auto table = std::ranges::find_if(tables_, [&](const auto& value) { return value->program == *program; });
        if (table == tables_.end())
        {
            tables_.push_back(std::make_unique<Table>(session_, input.program));
            table = std::prev(tables_.end());
        }
        const auto alignment = std::max<VkDeviceSize>(
            16,
            session_.physicalDevice().properties().properties.limits.minStorageBufferOffsetAlignment
        );
        auto range = (*table)->arena.allocate(backing->bytes, alignment);
        if (!range)
        {
            co_return cxx::unexpected(range.error());
        }
        backing->range = std::move(*range);
        backing->revision = revision + 1;
        backing->retirement = session_.retirement().reserve();
        auto id = input.material;
        if (id.isNull())
        {
            id.local = entries_.emplace();
        }
        entries_.find(id.local)->updating = true;
        Candidate candidate{*this, id, std::move(backing)};
        auto uploaded = co_await session_.uploads().writeBufferRange(
            *candidate.backing->range->buffer,
            candidate.backing->range->offset,
            candidate.backing->record
        );
        if (!uploaded)
        {
            co_return cxx::unexpected(uploaded.error());
        }
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        if (stop.stop_requested())
        {
            co_return cxx::unexpected(RenderError{kWorkStopped});
        }
        candidate.publish();
        co_return MaterialPublished{id, revision + 1};
    }

    RenderResult<MaterialInformation> MaterialStorage::query(MaterialId id) const noexcept
    {
        const auto* entry = id.storage == id_ ? entries_.find(id.local) : nullptr;
        if (!entry || !entry->current)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        const auto& value = *entry->current;
        return MaterialInformation{{id, value.revision}, value.program, value.parameters, value.textures};
    }

    RenderResult<void> MaterialStorage::release(MaterialId id, std::uint64_t revision) noexcept
    {
        if (id.storage != id_)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        auto* entry = entries_.find(id.local);
        if (!entry || !entry->current)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        if (entry->updating)
        {
            return cxx::unexpected(RenderError{kBusy});
        }
        if (revision != entry->current->revision)
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        disconnect(*entry);
        retire(entry->current);
        entries_.erase(id.local);
        return {};
    }

    RenderResult<void> MaterialStorage::prepare(
        const NativeResourcePlan& plan,
        const MaterialBinding& argument,
        PreparedStorageBinding& binding
    ) noexcept
    {
        auto* entry = argument.material.storage == id_ ? entries_.find(argument.material.local) : nullptr;
        if (!entry || !entry->current)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        const auto& current = *entry->current;
        const auto* program = &argument.program;
        if (*program != current.program)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        if (argument.texture_slot == UINT32_MAX)
        {
            const auto* description = std::get_if<GraphBufferDesc>(&plan.description);
            if (!description || description->size != current.bytes)
            {
                return cxx::unexpected(RenderError{kStorageMismatch});
            }
        }
        else
        {
            if (argument.texture_slot >= program->texture_count)
            {
                return cxx::unexpected(RenderError{kStorageMismatch});
            }
            const auto* description = std::get_if<GraphImageDesc>(&plan.description);
            const auto& actual = current.texture_info[argument.texture_slot];
            const auto format =
                actual.pixel_format == rdesc::ETexturePixelFormat::RGBA8_UNORM  ? rdesc::ETextureFormat::RGBA8_UNORM
                : actual.pixel_format == rdesc::ETexturePixelFormat::RGBA8_SRGB ? rdesc::ETextureFormat::RGBA8_SRGB
                                                                                : rdesc::ETextureFormat::BC1_RGBA_SRGB;
            const bool has_texture = actual.width != 0;
            const bool mismatch =
                !description ||
                (has_texture && (description->width != actual.width || description->height != actual.height ||
                                 description->mips != actual.mip_count || description->layers != actual.layers ||
                                 description->format != format));
            if (mismatch)
            {
                return cxx::unexpected(RenderError{kStorageMismatch});
            }
        }
        auto use = std::make_unique<Use>(this, argument.material, binding, entry->uses.size(), argument.texture_slot);
        entry->uses.push_back(use.get());
        use->update(entry->current);
        binding.reference = {use.release(), [](void* value) noexcept { delete static_cast<Use*>(value); }};
        return {};
    }

    void MaterialStorage::refreshTextures(MaterialData& data) noexcept
    {
        for (std::size_t i = 0; i < data.texture_bindings.size(); ++i)
        {
            const auto parameters = data.texture_bindings[i].parameters;
            if (parameters.empty())
            {
                continue;
            }
            float descriptor;
            std::memcpy(&descriptor, parameters.data(), sizeof(descriptor));
            data.setTexture(i, static_cast<std::uint32_t>(descriptor));
        }
        auto& entry = *entries_.find(data.id.local);
        for (auto* use : entry.uses)
        {
            use->update(entry.current);
        }
    }

    void MaterialStorage::retire(std::unique_ptr<MaterialData>& value) noexcept
    {
        if (value)
        {
            for (auto& texture : value->texture_bindings)
            {
                texture.updated = nullptr;
                texture.reference.reset();
            }
        }
        if (value && !BufferArena::retireRange(session_.retirement(), value->retirement, value->range))
        {
            std::terminate(); // The reservation was acquired with this physical candidate.
        }
        value.reset();
    }

    void MaterialStorage::unsubscribe(Use& use) noexcept
    {
        auto& subscriptions = entries_.find(use.id.local)->uses;
        subscriptions[use.position] = subscriptions.back();
        subscriptions[use.position]->position = use.position;
        subscriptions.pop_back();
    }

    void MaterialStorage::disconnect(Entry& entry) noexcept
    {
        for (auto* use : entry.uses)
        {
            use->storage = nullptr;
        }
        entry.uses.clear();
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
            [](void* object, const NativeResourcePlan& plan, const void* argument, PreparedStorageBinding& prepared
            ) noexcept
            {
                return static_cast<MaterialStorage*>(object)
                    ->prepare(plan, *static_cast<const MaterialBinding*>(argument), prepared);
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
