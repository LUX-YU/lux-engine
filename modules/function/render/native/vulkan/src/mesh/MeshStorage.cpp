#include <lux/engine/render/native/vulkan/mesh/MeshStorage.hpp>
#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>
#include <lux/engine/render/native/vulkan/StorageBinding.hpp>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include "Shader.hpp"
#include <algorithm>
#include <cstring>
#include <limits>

namespace lux::render::vulkan
{
    struct MeshStorage::Physical final
    {
        explicit Physical(DeviceSession& value) noexcept : session(value) {}
        ~Physical() noexcept { if (versions || bytes) std::terminate(); }
        DeviceSession& session;
        std::size_t versions = 0;
        std::uint64_t bytes = 0;
    };

    struct MeshStorage::Backing final
    {
        Backing(std::shared_ptr<Physical> owner, const MeshDescription& input, std::uint64_t size) noexcept
            : physical(std::move(owner)), description(input), bytes(size)
        {
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
        MeshDescription description;
        BufferPermit buffer;
        SubmissionRequirements last_use;
        RetirementReservation retirement;
        std::uint64_t bytes = 0, revision = 0;
        std::size_t position = 0, references = 0;
        bool live = false;
    };

    struct MeshStorage::Use final
    {
        MeshStorage& storage;
        MeshId id;
        Backing* backing;
        PreparedStorageBinding& binding;

        void update(Backing& value) noexcept
        {
            if (backing) storage.releaseReference(*backing);
            backing = &value;
            ++backing->references;
            binding.buffer = storage.session_.borrow(value.buffer);
            binding.last_use = &value.last_use;
            binding.publishUpdate();
        }

        ~Use() noexcept
        {
            storage.releaseReference(*backing);
            std::erase(storage.uses_, this);
        }
    };

    struct MeshStorage::Candidate final
    {
        MeshStorage& storage;
        MeshId id;
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

    MeshStorage::MeshStorage(DeviceSession& session, StorageId id, MeshStorageConfig config) noexcept
        : session_(session), id_(id), config_(config), physical_(std::make_shared<Physical>(session)) {}

    MeshStorage::~MeshStorage() noexcept
    {
        if (!uses_.empty()) std::terminate();
        while (!backings_.empty()) erase(*backings_.back());
    }

    std::uint64_t MeshStorage::residentBytes() const noexcept { return physical_->bytes; }

    std::size_t MeshStorage::residentVersions() const noexcept { return physical_->versions; }

    RenderResult<void> MeshStorage::validate(
        const MeshDescription& description, std::span<const std::byte> source
    ) const noexcept
    {
        const auto& limits = session_.device().properties().limits;
        const bool invalid_vertices = !description.vertex_count || !description.vertex_stride ||
            description.vertex_stride > limits.maxVertexInputBindingStride || description.vertex_stride % 4;
        const bool invalid_indices = !description.index_count ||
            (description.index_stride != 2 && description.index_stride != 4);
        const bool invalid_attributes = description.attributes.empty() ||
            description.attributes.size() > limits.maxVertexInputAttributes;
        if (invalid_vertices || invalid_indices || invalid_attributes || description.submeshes.empty())
            return cxx::unexpected(RenderError{kStorageContract});
        const std::uint64_t vertex_bytes = std::uint64_t(description.vertex_count) * description.vertex_stride;
        const std::uint64_t index_bytes = std::uint64_t(description.index_count) * description.index_stride;
        const auto total = cxx::checkedAdd(description.index_offset, index_bytes);
        if (!total) return cxx::unexpected(RenderError{kStorageContract});
        const auto padded = cxx::checkedAdd(*total, std::uint64_t{3});
        const bool invalid_range = description.index_offset < vertex_bytes ||
            description.index_offset % description.index_stride || !padded ||
            (*padded & ~std::uint64_t{3}) != source.size();
        if (invalid_range) return cxx::unexpected(RenderError{kStorageContract});
        for (std::size_t i = 0; i < description.attributes.size(); ++i)
        {
            const auto& attribute = description.attributes[i];
            const auto format = vertexFormat(attribute.format);
            const bool invalid_attribute = format.native == VK_FORMAT_UNDEFINED ||
                attribute.location >= limits.maxVertexInputAttributes || attribute.offset % 4 ||
                attribute.offset > limits.maxVertexInputAttributeOffset ||
                attribute.offset > description.vertex_stride ||
                format.bytes() > description.vertex_stride - attribute.offset;
            if (invalid_attribute) return cxx::unexpected(RenderError{kStorageContract});
            for (std::size_t j = 0; j < i; ++j)
                if (description.attributes[j].location == attribute.location)
                    return cxx::unexpected(RenderError{kStorageContract});
            VkFormatProperties supported{};
            vkGetPhysicalDeviceFormatProperties(session_.device().physical(), format.native, &supported);
            if (!(supported.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT))
                return cxx::unexpected(RenderError{kUnsupported});
        }
        for (const auto& submesh : description.submeshes)
        {
            const bool invalid_submesh = !submesh.index_count || submesh.index_count % 3 ||
                submesh.first_index > description.index_count ||
                submesh.index_count > description.index_count - submesh.first_index;
            if (invalid_submesh) return cxx::unexpected(RenderError{kStorageContract});
            for (std::uint32_t i = 0; i < submesh.index_count; ++i)
            {
                const auto* bytes = source.data() + description.index_offset +
                    std::uint64_t(submesh.first_index + i) * description.index_stride;
                std::uint32_t index = 0;
                if (description.index_stride == 2)
                {
                    std::uint16_t narrow = 0;
                    std::memcpy(&narrow, bytes, sizeof(narrow));
                    index = narrow;
                }
                else std::memcpy(&index, bytes, sizeof(index));
                const auto vertex = std::int64_t(index) + submesh.vertex_offset;
                if (vertex < 0 || vertex >= description.vertex_count)
                    return cxx::unexpected(RenderError{kStorageContract});
            }
        }
        return {};
    }

    MeshStorage::UploadResult MeshStorage::upload(const MeshWrite& input) noexcept
    {
        if (input.mesh.storage != id_) return cxx::unexpected(RenderError{kStorageIdentity});
        auto* existing = input.mesh.isNull() ? nullptr : entries_.find(input.mesh.local);
        if (!input.mesh.isNull() && !existing) return cxx::unexpected(RenderError{kStorageIdentity});
        if (existing && existing->updating) return cxx::unexpected(RenderError{kBusy});
        const auto revision = existing ? existing->current->revision : 0;
        if (input.expected_revision != revision || revision == std::numeric_limits<std::uint64_t>::max())
            return cxx::unexpected(RenderError{kStorageContract});
        const auto source = input.source.bytes().view();
        if (auto valid = validate(input.description, source); !valid) return cxx::unexpected(valid.error());
        // Immutable Graph recipes own counts/offsets. A live binding cannot silently change that ABI.
        if (existing && existing->current->references && existing->current->description != input.description)
            return cxx::unexpected(RenderError{kStorageMismatch});
        if (auto collected = session_.collect(); !collected) return cxx::unexpected(collected.error());
        const auto projected = cxx::checkedAdd(physical_->bytes, std::uint64_t(source.size()));
        const bool over_budget = !projected ||
            (config_.resident_byte_budget && *projected > config_.resident_byte_budget);
        if (over_budget) return cxx::unexpected(RenderError{kStorageCapacity});
        auto retirement = session_.retirement().reserve();
        if (!retirement) return cxx::unexpected(retirement.error());
        auto lease = session_.resources().lease(id_);
        if (!lease) return cxx::unexpected(lease.error());
        auto transfer = session_.uploadBuffer(source, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        if (!transfer) return cxx::unexpected(transfer.error());
        auto backing = std::make_unique<Backing>(physical_, input.description, source.size());
        backing->retirement = std::move(*retirement);
        backing->position = backings_.size();
        backing->revision = revision + 1;
        auto id = input.mesh;
        if (id.isNull()) id.local = entries_.emplace();
        entries_.find(id.local)->updating = true;
        auto* value = backing.get();
        backings_.push_back(std::move(backing));
        auto candidate = std::make_unique<Candidate>(*this, id, value, std::move(*lease));
        auto work = GpuWork::pending<[](Candidate& value, Transfer& transfer, DeviceSession&) noexcept {
            return value.publish(transfer);
        }>(session_, *transfer, candidate);
        if (!work) return cxx::unexpected(work.error());
        return std::pair{MeshPublished{id, revision + 1}, std::move(*work)};
    }

    RenderResult<MeshInformation> MeshStorage::query(MeshId id) const noexcept
    {
        const auto* entry = id.storage == id_ ? entries_.find(id.local) : nullptr;
        if (!entry || !entry->current) return cxx::unexpected(RenderError{kStorageIdentity});
        const auto& value = *entry->current;
        return MeshInformation{{id, value.revision}, value.description, value.bytes};
    }

    RenderResult<void> MeshStorage::release(MeshId id, std::uint64_t revision) noexcept
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

    RenderResult<void> MeshStorage::prepare(
        const NativeResourcePlan& plan, const MeshBinding& argument, PreparedStorageBinding& binding
    ) noexcept
    {
        const auto* entry = argument.mesh.storage == id_ ? entries_.find(argument.mesh.local) : nullptr;
        if (!entry || !entry->current) return cxx::unexpected(RenderError{kStorageIdentity});
        const auto* description = std::get_if<GraphBufferDesc>(&plan.description);
        const bool is_mismatch = !description || description->size != entry->current->bytes ||
            argument.description != entry->current->description;
        if (is_mismatch)
            return cxx::unexpected(RenderError{kStorageMismatch});
        auto use = std::make_unique<Use>(*this, argument.mesh, nullptr, binding);
        use->update(*entry->current);
        uses_.push_back(use.get());
        binding.reference = {use.release(), [](void* value) noexcept { delete static_cast<Use*>(value); }};
        return {};
    }

    void MeshStorage::releaseReference(Backing& value) noexcept
    {
        --value.references;
        retireUnused(value);
    }

    void MeshStorage::retireUnused(Backing& value) noexcept
    {
        if (!value.live && !value.references) erase(value);
    }

    void MeshStorage::erase(Backing& value) noexcept
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
        if (owned->last_use.completed()) return;
        auto result = session_.retirement().retire(owned->retirement, owned->last_use, owned);
        if (!result) std::terminate();
    }

    StorageFactory meshStorageFactory() noexcept
    {
        static const render::MeshStorage::Ops ops{
            [](const void* object, MeshId id) noexcept { return static_cast<const MeshStorage*>(object)->query(id); },
            [](void* object, MeshId id, std::uint64_t revision) noexcept {
                return static_cast<MeshStorage*>(object)->release(id, revision);
            }
        };
        static const StorageBindingOps binding{
            [](void* object, const NativeResourcePlan& plan, const void* argument,
                PreparedStorageBinding& prepared) noexcept {
                return static_cast<MeshStorage*>(object)->prepare(
                    plan, *static_cast<const MeshBinding*>(argument), prepared
                );
            }
        };
        static const auto create = +[](DeviceSession& session, StorageId id, const MeshStorageConfig& config) noexcept
            -> RenderResult<std::unique_ptr<MeshStorage>> {
            return std::make_unique<MeshStorage>(session, id, config);
        };
        static const auto compatible = +[](const MeshStorage& value, const MeshStorageConfig& config) noexcept {
            return value.compatibleConfig(config);
        };
        return makeStorageFactory<render::MeshStorage, MeshStorage, DeviceSession, create, compatible>(ops, &binding);
    }
}

namespace lux::render
{
    StorageTraits<vulkan::MeshStorage>::Result StorageTraits<vulkan::MeshStorage>::upload(
        vulkan::MeshStorage& storage, const MeshWrite& input
    ) noexcept
    {
        return storage.upload(input);
    }
}
