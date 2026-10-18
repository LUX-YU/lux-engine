#include "Shader.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/StorageBinding.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshStorage.hpp>

namespace lux::render::vulkan
{
    struct MeshStorage::Residency final
    {
        ~Residency() noexcept
        {
            if (versions || bytes)
            {
                std::terminate();
            }
        }

        std::size_t versions = 0;
        std::uint64_t bytes = 0;
    };

    struct MeshStorage::MeshData final
    {
        MeshData(std::shared_ptr<Residency> owner, const MeshDescription& input, std::uint64_t size) noexcept
            : residency(std::move(owner)), description(input), bytes(size)
        {
            ++residency->versions;
            residency->bytes += bytes;
        }

        ~MeshData() noexcept
        {
            --residency->versions;
            residency->bytes -= bytes;
        }

        std::shared_ptr<Residency> residency;
        MeshDescription description;
        std::shared_ptr<Buffer> buffer;
        SubmissionRequirements last_use;
        std::uint64_t bytes = 0, revision = 0;
    };

    struct MeshStorage::Use final
    {
        MeshStorage* storage;
        MeshId id;
        PreparedStorageBinding& binding;
        std::size_t position;

        void update(const std::shared_ptr<MeshData>& value) noexcept
        {
            binding.resources = value;
            binding.buffer = value->buffer.get();
            binding.last_use = &value->last_use;
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

    struct MeshStorage::Candidate final
    {
        MeshStorage& storage;
        MeshId id;
        std::shared_ptr<MeshData> backing;
        std::shared_ptr<void> storage_reference;

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
        }

        RenderResult<void> publish(const SubmissionRequirements& requirements) noexcept
        {
            if (!storage.session_.resources().native(storage.id_))
            {
                return cxx::unexpected(RenderError{kStorageIdentity});
            }
            backing->last_use = requirements;
            auto& entry = *storage.entries_.find(id.local);
            entry.current = std::move(backing);
            entry.updating = false;
            for (auto* use : entry.uses)
            {
                use->update(entry.current);
            }
            return {};
        }
    };

    MeshStorage::MeshStorage(DeviceSession& session, StorageId id, MeshStorageConfig config) noexcept
        : session_(session), id_(id), config_(config), residency_(std::make_shared<Residency>())
    {
    }

    MeshStorage::~MeshStorage() noexcept
    {
        for (auto& entry : entries_)
        {
            disconnect(entry);
        }
    }

    std::uint64_t MeshStorage::residentBytes() const noexcept
    {
        return residency_->bytes;
    }

    std::size_t MeshStorage::residentVersions() const noexcept
    {
        return residency_->versions;
    }

    RenderResult<void> MeshStorage::validate(const MeshDescription& description, std::span<const std::byte> source)
        const noexcept
    {
        const auto& limits = session_.device().properties().limits;
        const bool invalid_vertices = !description.vertex_count || !description.vertex_stride ||
                                      description.vertex_stride > limits.maxVertexInputBindingStride ||
                                      description.vertex_stride % 4;
        const bool invalid_indices =
            !description.index_count || (description.index_stride != 2 && description.index_stride != 4);
        const bool invalid_attributes =
            description.attributes.empty() || description.attributes.size() > limits.maxVertexInputAttributes;
        if (invalid_vertices || invalid_indices || invalid_attributes || description.submeshes.empty())
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        const std::uint64_t vertex_bytes = std::uint64_t(description.vertex_count) * description.vertex_stride;
        const std::uint64_t index_bytes = std::uint64_t(description.index_count) * description.index_stride;
        const auto total = cxx::checkedAdd(description.index_offset, index_bytes);
        if (!total)
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        const auto padded = cxx::checkedAdd(*total, std::uint64_t{3});
        const bool invalid_range = description.index_offset < vertex_bytes ||
                                   description.index_offset % description.index_stride || !padded ||
                                   (*padded & ~std::uint64_t{3}) != source.size();
        if (invalid_range)
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        for (std::size_t i = 0; i < description.attributes.size(); ++i)
        {
            const auto& attribute = description.attributes[i];
            const auto format = vertexFormat(attribute.format);
            const bool invalid_attribute =
                format.native == VK_FORMAT_UNDEFINED || attribute.location >= limits.maxVertexInputAttributes ||
                attribute.offset % 4 || attribute.offset > limits.maxVertexInputAttributeOffset ||
                attribute.offset > description.vertex_stride ||
                format.bytes() > description.vertex_stride - attribute.offset;
            if (invalid_attribute)
            {
                return cxx::unexpected(RenderError{kStorageContract});
            }
            for (std::size_t j = 0; j < i; ++j)
            {
                if (description.attributes[j].location == attribute.location)
                {
                    return cxx::unexpected(RenderError{kStorageContract});
                }
            }
            VkFormatProperties supported{};
            vkGetPhysicalDeviceFormatProperties(session_.device().physical(), format.native, &supported);
            if (!(supported.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT))
            {
                return cxx::unexpected(RenderError{kUnsupported});
            }
        }
        for (const auto& submesh : description.submeshes)
        {
            const bool invalid_submesh = !submesh.index_count || submesh.index_count % 3 ||
                                         submesh.first_index > description.index_count ||
                                         submesh.index_count > description.index_count - submesh.first_index;
            if (invalid_submesh)
            {
                return cxx::unexpected(RenderError{kStorageContract});
            }
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
                else
                {
                    std::memcpy(&index, bytes, sizeof(index));
                }
                const auto vertex = std::int64_t(index) + submesh.vertex_offset;
                if (vertex < 0 || vertex >= description.vertex_count)
                {
                    return cxx::unexpected(RenderError{kStorageContract});
                }
            }
        }
        return {};
    }

    MeshStorage::UploadResult MeshStorage::upload(MeshWrite input) noexcept
    {
        if (input.mesh.storage != id_)
        {
            co_return cxx::unexpected(RenderError{kStorageIdentity});
        }
        auto* existing = input.mesh.isNull() ? nullptr : entries_.find(input.mesh.local);
        if (!input.mesh.isNull() && !existing)
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
        const auto source = input.source.bytes().view();
        if (auto valid = validate(input.description, source); !valid)
        {
            co_return cxx::unexpected(valid.error());
        }
        // Immutable Graph recipes own counts/offsets. A live binding cannot silently change that ABI.
        if (existing && !existing->uses.empty() && existing->current->description != input.description)
        {
            co_return cxx::unexpected(RenderError{kStorageMismatch});
        }
        if (auto collected = session_.collect(); !collected)
        {
            co_return cxx::unexpected(collected.error());
        }
        const auto projected = cxx::checkedAdd(residency_->bytes, std::uint64_t(source.size()));
        const bool over_budget =
            !projected || (config_.resident_byte_budget && *projected > config_.resident_byte_budget);
        if (over_budget)
        {
            co_return cxx::unexpected(RenderError{kStorageCapacity});
        }
        auto storage_reference = session_.resources().share(id_);
        if (!storage_reference)
        {
            co_return cxx::unexpected(storage_reference.error());
        }
        auto buffer = session_.allocator().createBuffer(
            source.size(),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            EMemoryAccess::DEVICE
        );
        if (!buffer)
        {
            co_return cxx::unexpected(buffer.error());
        }
        auto backing = std::make_shared<MeshData>(residency_, input.description, source.size());
        backing->buffer = std::move(*buffer);
        backing->revision = revision + 1;
        auto id = input.mesh;
        if (id.isNull())
        {
            id.local = entries_.emplace();
        }
        entries_.find(id.local)->updating = true;
        auto candidate = std::make_unique<Candidate>(*this, id, std::move(backing), std::move(*storage_reference));
        auto uploaded = co_await session_.uploads().writeBuffer(candidate->backing->buffer, std::move(input.source));
        if (!uploaded)
        {
            co_return cxx::unexpected(uploaded.error());
        }
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        if (stop.stop_requested())
        {
            co_return cxx::unexpected(RenderError{kOutcomeUnknown});
        }
        if (auto published = candidate->publish(*uploaded); !published)
        {
            co_return cxx::unexpected(published.error());
        }
        co_return MeshPublished{id, revision + 1};
    }

    RenderResult<MeshInformation> MeshStorage::query(MeshId id) const noexcept
    {
        const auto* entry = id.storage == id_ ? entries_.find(id.local) : nullptr;
        if (!entry || !entry->current)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        const auto& value = *entry->current;
        return MeshInformation{{id, value.revision}, value.description, value.bytes};
    }

    RenderResult<void> MeshStorage::release(MeshId id, std::uint64_t revision) noexcept
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
        entries_.erase(id.local);
        return {};
    }

    RenderResult<void> MeshStorage::prepare(
        const NativeResourcePlan& plan,
        const MeshBinding& argument,
        PreparedStorageBinding& binding
    ) noexcept
    {
        auto* entry = argument.mesh.storage == id_ ? entries_.find(argument.mesh.local) : nullptr;
        if (!entry || !entry->current)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        const auto* description = std::get_if<GraphBufferDesc>(&plan.description);
        const bool is_mismatch = !description || description->size != entry->current->bytes ||
                                 argument.description != entry->current->description;
        if (is_mismatch)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        auto use = std::make_unique<Use>(this, argument.mesh, binding, entry->uses.size());
        entry->uses.push_back(use.get());
        use->update(entry->current);
        binding.reference = {use.release(), [](void* value) noexcept { delete static_cast<Use*>(value); }};
        return {};
    }

    void MeshStorage::unsubscribe(Use& use) noexcept
    {
        auto& subscriptions = entries_.find(use.id.local)->uses;
        subscriptions[use.position] = subscriptions.back();
        subscriptions[use.position]->position = use.position;
        subscriptions.pop_back();
    }

    void MeshStorage::disconnect(Entry& entry) noexcept
    {
        for (auto* use : entry.uses)
        {
            use->storage = nullptr;
        }
        entry.uses.clear();
    }

    StorageFactory meshStorageFactory() noexcept
    {
        static const render::MeshStorage::Ops ops{
            [](const void* object, MeshId id) noexcept { return static_cast<const MeshStorage*>(object)->query(id); },
            [](void* object, MeshId id, std::uint64_t revision) noexcept
            { return static_cast<MeshStorage*>(object)->release(id, revision); }
        };
        static const StorageBindingOps binding{
            [](void* object, const NativeResourcePlan& plan, const void* argument, PreparedStorageBinding& prepared
            ) noexcept
            {
                return static_cast<MeshStorage*>(object)
                    ->prepare(plan, *static_cast<const MeshBinding*>(argument), prepared);
            }
        };
        static const auto create = +[](DeviceSession& session, StorageId id, const MeshStorageConfig& config
                                    ) noexcept -> RenderResult<std::unique_ptr<MeshStorage>>
        { return std::make_unique<MeshStorage>(session, id, config); };
        static const auto compatible = +[](const MeshStorage& value, const MeshStorageConfig& config) noexcept
        { return value.compatibleConfig(config); };
        return makeStorageFactory<render::MeshStorage, MeshStorage, DeviceSession, create, compatible>(ops, &binding);
    }
} // namespace lux::render::vulkan

namespace lux::render
{
    StorageTraits<vulkan::MeshStorage>::Result StorageTraits<vulkan::MeshStorage>::upload(
        vulkan::MeshStorage& storage,
        MeshWrite input
    ) noexcept
    {
        return storage.upload(std::move(input));
    }
} // namespace lux::render
