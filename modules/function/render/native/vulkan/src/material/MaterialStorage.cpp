#include <lux/engine/render/native/vulkan/material/MaterialStorage.hpp>
#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>
#include <lux/engine/render/native/vulkan/StorageBinding.hpp>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <cmath>
#include <lux/engine/render/native/vulkan/texture/TextureStorage.hpp>
#include <algorithm>
#include <cstring>
#include <limits>

namespace lux::render::vulkan
{
    struct MaterialStorage::Physical final
    {
        explicit Physical(DeviceSession& value) noexcept : session(value) {}
        ~Physical() noexcept { if (versions || bytes) std::terminate(); }
        DeviceSession& session;
        std::size_t versions = 0;
        std::uint64_t bytes = 0;
    };

    struct MaterialStorage::Backing final
    {
        // Existing Cooked GraphMaterialGPU ABI: 32-byte header, sixteen vec4 parameters, eight texture refs.
        struct alignas(16) Record final
        {
            std::array<std::uint32_t, 8> header{};
            MaterialParameters parameters{};
            std::array<std::array<std::uint32_t, 4>, 8> textures{};
        };
        static_assert(sizeof(Record) == 416);
        Backing(std::shared_ptr<Physical> owner, const MaterialWrite& input) noexcept
            : physical(std::move(owner)), program(input.program), textures(input.textures), bytes(sizeof(Record))
        {
            record.parameters = input.parameters;
            ++physical->versions;
            physical->bytes += bytes;
        }

        ~Backing() noexcept
        {
            if (buffer && !physical->session.releaseCompleted(buffer, last_use)) std::terminate();
            --physical->versions;
            physical->bytes -= bytes;
        }

        [[nodiscard]] VkDevice device() const noexcept { return physical->session.device().native(); }
        std::shared_ptr<Physical> physical;
        ConfigValue program;
        std::array<TexturePublished, 8> textures;
        std::array<rdesc::TextureInfo, 8> texture_info;
        // Declaration order: Native texture references die before their logical Storage leases.
        std::array<StorageLease, 8> texture_leases;
        std::array<PreparedStorageBinding, 8> texture_bindings;
        Record record;
        BufferPermit buffer;
        SubmissionRequirements last_use;
        RetirementReservation retirement;
        std::uint64_t bytes = 0, revision = 0;
        std::size_t position = 0, references = 0;
        bool live = false;
    };

    struct MaterialStorage::Use final
    {
        MaterialStorage& storage;
        MaterialId id;
        Backing* backing;
        PreparedStorageBinding& binding;
        std::uint32_t texture_slot;

        void update(Backing& value) noexcept
        {
            if (backing) storage.releaseReference(*backing);
            backing = &value;
            ++backing->references;
            if (texture_slot == UINT32_MAX)
            {
                binding.buffer = storage.session_.borrow(value.buffer);
                binding.last_use = &value.last_use;
            }
            else
            {
                binding.descriptors = value.texture_bindings[texture_slot].descriptors;
                binding.last_use = value.texture_bindings[texture_slot].last_use;
            }
            binding.publishUpdate();
        }

        ~Use() noexcept
        {
            storage.releaseReference(*backing);
            std::erase(storage.uses_, this);
        }
    };

    struct MaterialStorage::Candidate final
    {
        MaterialStorage& storage;
        MaterialId id;
        Backing* backing;
        StorageLease lease;
        bool published = false;

        ~Candidate() noexcept
        {
            if (published) return;
            auto& entry = *storage.entries_.find(id.local);
            entry.updating = false;
            if (!entry.current) storage.entries_.erase(id.local);
            storage.erase(*backing);
        }

        RenderResult<void> publish(Transfer& transfer) noexcept
        {
            auto buffer = transfer.takeBuffer();
            if (!buffer) return cxx::unexpected(buffer.error());
            backing->buffer = std::move(*buffer);
            backing->last_use = transfer.requirements();
            auto& entry = *storage.entries_.find(id.local);
            auto* previous = entry.current;
            entry.current = backing;
            entry.updating = false;
            backing->live = true;
            if (previous)
            {
                previous->live = false;
                storage.retireUnused(*previous);
            }
            for (auto* use : storage.uses_)
                if (use->id == id) use->update(*backing);
            published = true;
            return {};
        }
    };

    MaterialStorage::MaterialStorage(DeviceSession& session, StorageId id, MaterialStorageConfig config) noexcept
        : session_(session), id_(id), config_(config), physical_(std::make_shared<Physical>(session)) {}

    MaterialStorage::~MaterialStorage() noexcept
    {
        if (!uses_.empty()) std::terminate();
        while (!backings_.empty()) erase(*backings_.back());
    }

    std::uint64_t MaterialStorage::residentBytes() const noexcept { return physical_->bytes; }

    std::size_t MaterialStorage::residentVersions() const noexcept { return physical_->versions; }

    MaterialStorage::UploadResult MaterialStorage::upload(const MaterialWrite& input) noexcept
    {
        if (input.material.storage != id_) return cxx::unexpected(RenderError{kStorageIdentity});
        auto* existing = input.material.isNull() ? nullptr : entries_.find(input.material.local);
        if (!input.material.isNull() && !existing) return cxx::unexpected(RenderError{kStorageIdentity});
        if (existing && existing->updating) return cxx::unexpected(RenderError{kBusy});
        const auto revision = existing ? existing->current->revision : 0;
        if (input.expected_revision != revision || revision == std::numeric_limits<std::uint64_t>::max())
            return cxx::unexpected(RenderError{kStorageContract});
        const auto* program = input.program.borrow<MaterialProgram>();
        const bool invalid_program = !program || program->fragment.stage != rdesc::EShaderType::FRAGMENT ||
            program->fragment.entry.empty() || program->fragment.words.size() < 5 ||
            program->fragment.words.front() != 0x07230203 || program->parameter_count > 16 || program->texture_count > 8;
        if (invalid_program) return cxx::unexpected(RenderError{kStorageContract});
        for (std::size_t slot = program->texture_count; slot < input.textures.size(); ++slot)
            if (!input.textures[slot].texture.isNull() || input.textures[slot].revision)
                return cxx::unexpected(RenderError{kStorageContract});
        for (const auto& lane : input.parameters)
            for (const auto value : lane)
                if (!std::isfinite(value)) return cxx::unexpected(RenderError{kStorageContract});
        // Only the cold Graph owns a Pipeline. A bound Material cannot silently replace its Shader contract.
        if (existing && existing->current->references &&
            *existing->current->program.borrow<MaterialProgram>() != *program)
            return cxx::unexpected(RenderError{kStorageMismatch});
        if (auto collected = session_.collect(); !collected) return cxx::unexpected(collected.error());
        const auto projected = cxx::checkedAdd(physical_->bytes, std::uint64_t(sizeof(Backing::Record)));
        const bool over_budget = !projected ||
            (config_.resident_byte_budget && *projected > config_.resident_byte_budget);
        if (over_budget) return cxx::unexpected(RenderError{kStorageCapacity});
        auto retirement = session_.retirement().reserve();
        if (!retirement) return cxx::unexpected(retirement.error());
        auto lease = session_.resources().lease(id_);
        if (!lease) return cxx::unexpected(lease.error());
        auto backing = std::make_unique<Backing>(physical_, input);
        for (std::uint32_t slot = 0; slot < program->texture_count; ++slot)
        {
            const auto texture = input.textures[slot];
            auto& registry = session_.resources();
            auto* owner = registry.owner(texture.texture.storage);
            const bool invalid_owner = owner != &session_.globalStorages() && owner != registry.owner(id_);
            if (!owner || invalid_owner) return cxx::unexpected(RenderError{kStorageIdentity});
            auto storage = registry.borrow<TextureStorage>(texture.texture.storage);
            if (!storage) return cxx::unexpected(storage.error());
            auto texture_lease = registry.lease(texture.texture.storage);
            if (!texture_lease) return cxx::unexpected(texture_lease.error());
            backing->texture_leases[slot] = std::move(*texture_lease);
            auto reference = (*storage)->pin(texture, backing->texture_bindings[slot]);
            if (!reference) return cxx::unexpected(reference.error());
            backing->texture_info[slot] = reference->first;
            backing->record.textures[slot] = {0, reference->second, 0, 0};
            backing->record.header[2] |= 1u << slot;
        }
        for (auto* use : uses_)
        {
            if (use->id != input.material || use->texture_slot == UINT32_MAX) continue;
            const auto& previous = use->backing->texture_info[use->texture_slot];
            const auto& next = backing->texture_info[use->texture_slot];
            const bool mismatch = previous.width != next.width || previous.height != next.height ||
                previous.mip_count != next.mip_count || previous.layers != next.layers ||
                previous.pixel_format != next.pixel_format;
            if (mismatch) return cxx::unexpected(RenderError{kStorageMismatch});
        }
        const auto source = std::as_bytes(std::span(&backing->record, 1));
        auto transfer = session_.uploadBuffer(source, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        if (!transfer) return cxx::unexpected(transfer.error());
        backing->retirement = std::move(*retirement);
        backing->position = backings_.size();
        backing->revision = revision + 1;
        auto id = input.material;
        if (id.isNull()) id.local = entries_.emplace();
        entries_.find(id.local)->updating = true;
        auto* value = backing.get();
        backings_.push_back(std::move(backing));
        auto candidate = std::make_unique<Candidate>(*this, id, value, std::move(*lease));
        auto work = GpuWork::pending<[](Candidate& value, Transfer& transfer, DeviceSession&) noexcept {
            return value.publish(transfer);
        }>(session_, *transfer, candidate);
        if (!work) return cxx::unexpected(work.error());
        return std::pair{MaterialPublished{id, revision + 1}, std::move(*work)};
    }

    RenderResult<MaterialInformation> MaterialStorage::query(MaterialId id) const noexcept
    {
        const auto* entry = id.storage == id_ ? entries_.find(id.local) : nullptr;
        if (!entry || !entry->current) return cxx::unexpected(RenderError{kStorageIdentity});
        const auto& value = *entry->current;
        return MaterialInformation{{id, value.revision}, value.program, value.record.parameters, value.textures};
    }

    RenderResult<void> MaterialStorage::release(MaterialId id, std::uint64_t revision) noexcept
    {
        if (id.storage != id_) return cxx::unexpected(RenderError{kStorageIdentity});
        auto* entry = entries_.find(id.local);
        if (!entry || !entry->current) return cxx::unexpected(RenderError{kStorageIdentity});
        if (entry->updating) return cxx::unexpected(RenderError{kBusy});
        if (revision != entry->current->revision) return cxx::unexpected(RenderError{kStorageContract});
        auto* value = entry->current;
        value->live = false;
        entries_.erase(id.local);
        retireUnused(*value);
        return {};
    }

    RenderResult<void> MaterialStorage::prepare(
        const NativeResourcePlan& plan, const MaterialBinding& argument, PreparedStorageBinding& binding
    ) noexcept
    {
        const auto* entry = argument.material.storage == id_ ? entries_.find(argument.material.local) : nullptr;
        if (!entry || !entry->current) return cxx::unexpected(RenderError{kStorageIdentity});
        const auto& current = *entry->current;
        const auto* program = argument.program.borrow<MaterialProgram>();
        if (!program || *program != *current.program.borrow<MaterialProgram>())
            return cxx::unexpected(RenderError{kStorageMismatch});
        if (argument.texture_slot == UINT32_MAX)
        {
            const auto* description = std::get_if<GraphBufferDesc>(&plan.description);
            if (!description || description->size != current.bytes)
                return cxx::unexpected(RenderError{kStorageMismatch});
        }
        else
        {
            if (argument.texture_slot >= program->texture_count)
                return cxx::unexpected(RenderError{kStorageMismatch});
            const auto* description = std::get_if<GraphImageDesc>(&plan.description);
            const auto& actual = current.texture_info[argument.texture_slot];
            const auto format = actual.pixel_format == rdesc::ETexturePixelFormat::RGBA8_UNORM ?
                rdesc::ETextureFormat::RGBA8_UNORM : actual.pixel_format == rdesc::ETexturePixelFormat::RGBA8_SRGB ?
                rdesc::ETextureFormat::RGBA8_SRGB : rdesc::ETextureFormat::BC1_RGBA_SRGB;
            const bool mismatch = !description || description->width != actual.width ||
                description->height != actual.height || description->mips != actual.mip_count ||
                description->layers != actual.layers || description->format != format;
            if (mismatch) return cxx::unexpected(RenderError{kStorageMismatch});
        }
        auto use = std::make_unique<Use>(*this, argument.material, nullptr, binding,
            argument.texture_slot);
        use->update(*entry->current);
        uses_.push_back(use.get());
        binding.reference = {use.release(), [](void* value) noexcept { delete static_cast<Use*>(value); }};
        return {};
    }

    void MaterialStorage::releaseReference(Backing& value) noexcept
    {
        --value.references;
        retireUnused(value);
    }

    void MaterialStorage::retireUnused(Backing& value) noexcept
    {
        if (!value.live && !value.references) erase(value);
    }

    void MaterialStorage::erase(Backing& value) noexcept
    {
        if (value.references) std::terminate();
        const auto position = value.position;
        auto owned = std::move(backings_[position]);
        if (position + 1 != backings_.size())
        {
            backings_[position] = std::move(backings_.back());
            backings_[position]->position = position;
        }
        backings_.pop_back();
        // Future material uses have ended. Texture's own LastUse keeps its physical backing safe;
        // do not keep logical Scene/View Storage leases inside the GPU retirement owner.
        for (auto& texture : owned->texture_bindings) texture.reference.reset();
        for (auto& lease : owned->texture_leases) lease.reset();
        if (owned->last_use.completed()) return;
        auto result = session_.retirement().retire(owned->retirement, owned->last_use, owned);
        if (!result) std::terminate();
    }

    StorageFactory materialStorageFactory() noexcept
    {
        static const render::MaterialStorage::Ops ops{
            [](const void* object, MaterialId id) noexcept { return static_cast<const MaterialStorage*>(object)->query(id); },
            [](void* object, MaterialId id, std::uint64_t revision) noexcept {
                return static_cast<MaterialStorage*>(object)->release(id, revision);
            }
        };
        static const StorageBindingOps binding{
            [](void* object, const NativeResourcePlan& plan, const void* argument,
                PreparedStorageBinding& prepared) noexcept {
                return static_cast<MaterialStorage*>(object)->prepare(
                    plan, *static_cast<const MaterialBinding*>(argument), prepared
                );
            }
        };
        static const auto create = +[](DeviceSession& session, StorageId id, const MaterialStorageConfig& config) noexcept
            -> RenderResult<std::unique_ptr<MaterialStorage>> {
            return std::make_unique<MaterialStorage>(session, id, config);
        };
        static const auto compatible = +[](const MaterialStorage& value, const MaterialStorageConfig& config) noexcept {
            return value.compatibleConfig(config);
        };
        return makeStorageFactory<render::MaterialStorage, MaterialStorage, DeviceSession, create, compatible>(ops, &binding);
    }
}

namespace lux::render
{
    StorageTraits<vulkan::MaterialStorage>::Result StorageTraits<vulkan::MaterialStorage>::upload(
        vulkan::MaterialStorage& storage, const MaterialWrite& input
    ) noexcept
    {
        return storage.upload(input);
    }
}
