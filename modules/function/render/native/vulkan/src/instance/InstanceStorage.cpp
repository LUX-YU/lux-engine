#include <lux/engine/render/native/vulkan/instance/InstanceStorage.hpp>
#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>
#include <lux/engine/render/native/vulkan/StorageBinding.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace lux::render::vulkan
{
    struct InstanceStorage::Physical final
    {
        DeviceSession& session;
        std::uint64_t bytes = 0;
        ~Physical() noexcept { if (bytes) std::terminate(); }
    };

    struct InstanceStorage::Backing final
    {
        explicit Backing(std::shared_ptr<Physical> value) noexcept : physical(std::move(value)) {}
        ~Backing() noexcept
        {
            if (buffer)
            {
                const auto released = last_use.empty() ? physical->session.destroyUnsubmitted(buffer) :
                    physical->session.releaseCompleted(buffer, last_use);
                if (!released) std::terminate();
            }
            physical->bytes -= bytes;
        }

        [[nodiscard]] VkDevice device() const noexcept { return physical->session.device().native(); }
        void mark(std::uint32_t index) noexcept
        {
            if (!dirty[index]) { dirty[index] = true; pending.push_back(index); }
        }

        std::shared_ptr<Physical> physical;
        BufferPermit buffer;
        SubmissionRequirements last_use;
        RetirementReservation retirement;
        std::vector<bool> dirty;
        std::vector<std::uint32_t> pending;
        std::uint64_t bytes = 0;
    };

    struct InstanceStorage::Use final
    {
        InstanceStorage& storage;
        PreparedStorageBinding& binding;
        ~Use() noexcept { std::erase(storage.uses_, this); }
        void update(Backing& value) noexcept
        {
            binding.buffer = storage.session_.borrow(value.buffer);
            binding.last_use = &value.last_use;
            binding.publishUpdate();
        }
    };

    InstanceStorage::InstanceStorage(DeviceSession& session, StorageId id, InstanceStorageConfig config) noexcept
        : session_(session), id_(id), config_(config), physical_(std::make_shared<Physical>(session)) {}

    InstanceStorage::~InstanceStorage() noexcept
    {
        if (!uses_.empty()) std::terminate();
        for (auto& backing : banks_) retire(backing);
    }

    std::uint64_t InstanceStorage::residentBytes() const noexcept { return physical_->bytes; }

    void InstanceStorage::retire(std::unique_ptr<Backing>& value) noexcept
    {
        if (!value) return;
        if (value->last_use.completed()) value.reset();
        else if (!session_.retirement().retire(value->retirement, value->last_use, value)) std::terminate();
    }

    RenderResult<void> InstanceStorage::grow(std::size_t count) noexcept
    {
        if (count <= shadow_.size()) return {};
        const auto maximum = session_.device().properties().limits.maxStorageBufferRange / sizeof(TransformMatrix);
        if (count > maximum) return cxx::unexpected(RenderError{kStorageCapacity});
        const auto capacity = std::min<std::size_t>(maximum, std::max(count, std::max(std::size_t{1}, shadow_.size() * 2)));
        const auto bytes = capacity * sizeof(TransformMatrix);
        const bool exceeds_budget = config_.resident_byte_budget &&
            (bytes * banks_.size() > config_.resident_byte_budget ||
                physical_->bytes > config_.resident_byte_budget - bytes * banks_.size());
        if (exceeds_budget) return cxx::unexpected(RenderError{kStorageCapacity});
        auto shadow = shadow_;
        shadow.resize(capacity);
        Banks candidate;
        for (auto& value : candidate)
        {
            auto reservation = session_.retirement().reserve();
            if (!reservation) return cxx::unexpected(reservation.error());
            value = std::make_unique<Backing>(physical_);
            value->retirement = std::move(*reservation);
            auto buffer = session_.createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT, EMemoryAccess::UPLOAD);
            if (!buffer) return cxx::unexpected(buffer.error());
            value->buffer = std::move(*buffer);
            value->bytes = bytes;
            physical_->bytes += bytes;
            value->dirty.resize(capacity);
            value->pending.reserve(capacity);
            auto result = session_.write(*session_.borrow(value->buffer), 0, std::as_bytes(std::span(shadow)));
            if (!result) return result;
            written_records_ += capacity;
        }
        shadow_ = std::move(shadow);
        // Rebind before retiring old pointers. Their recorded GPU tickets remain in the old backing.
        publish(*candidate.front());
        for (auto& value : banks_) retire(value);
        banks_ = std::move(candidate);
        return {};
    }

    RenderResult<InstanceStorage::Backing*> InstanceStorage::writable() noexcept
    {
        if (auto collected = session_.collect(); !collected) return cxx::unexpected(collected.error());
        for (auto& value : banks_)
            if (value && value.get() != current_ && value->last_use.completed()) return value.get();
        return cxx::unexpected(RenderError{kBusy});
    }

    RenderResult<void> InstanceStorage::write(Backing& backing, std::uint32_t index,
        const TransformMatrix& transform) noexcept
    {
        // Mark before write: even a failed flush is repaired from CPU authority on the next attempt.
        backing.mark(index);
        auto result = session_.write(*session_.borrow(backing.buffer), std::uint64_t(index) * sizeof(transform),
            std::as_bytes(std::span(&transform, 1)));
        if (result) ++written_records_;
        return result;
    }

    void InstanceStorage::publish(Backing& value) noexcept
    {
        current_ = &value;
        for (auto* use : uses_) use->update(value);
    }

    RenderResult<InstancePublished> InstanceStorage::create(const InstanceDescription& input) noexcept
    {
        if (!std::ranges::all_of(input.transform, [](float x) { return std::isfinite(x); }))
            return cxx::unexpected(RenderError{kStorageContract});
        auto& registry = session_.resources();
        for (auto source : {input.mesh.storage, input.material.storage})
        {
            auto* owner = registry.owner(source);
            if (!owner || (owner != registry.owner(id_) && owner != &session_.globalStorages()))
                return cxx::unexpected(RenderError{kStorageIdentity});
        }
        auto mesh = registry.tryGet<render::MeshStorage>(input.mesh.storage);
        if (!mesh) return cxx::unexpected(mesh.error());
        auto mesh_info = mesh->query(input.mesh);
        if (!mesh_info) return cxx::unexpected(mesh_info.error());
        if (input.submesh >= mesh_info->description.submeshes.size())
            return cxx::unexpected(RenderError{kStorageContract});
        auto material = registry.tryGet<render::MaterialStorage>(input.material.storage);
        if (!material) return cxx::unexpected(material.error());
        auto material_info = material->query(input.material);
        if (!material_info) return cxx::unexpected(material_info.error());
        const auto key = entries_.emplace(Entry{input, 0});
        if (auto result = grow(std::size_t(key.index) + 1); !result)
        {
            entries_.erase(key);
            return cxx::unexpected(result.error());
        }
        TransformBatch initial{id_, 0, {{{id_, key}, 0, input.transform}}};
        auto result = update(initial);
        if (!result)
        {
            entries_.erase(key);
            return cxx::unexpected(result.error());
        }
        return InstancePublished{{id_, key}, 1};
    }

    RenderResult<TransformApplied> InstanceStorage::update(const TransformBatch& input) noexcept
    {
        if (input.storage != id_ || input.updates.size() > UINT32_MAX)
            return cxx::unexpected(RenderError{kStorageIdentity});
        std::vector<std::size_t> order(input.updates.size());
        std::iota(order.begin(), order.end(), std::size_t{0});
        for (const auto& value : input.updates)
        {
            ++checked_updates_;
            const auto* entry = value.instance.storage == id_ ? entries_.find(value.instance.local) : nullptr;
            if (!entry) return cxx::unexpected(RenderError{kStorageIdentity});
            const bool revision_mismatch = value.expected_revision != entry->revision || entry->revision == UINT64_MAX;
            const bool invalid_transform = !std::ranges::all_of(value.transform, [](float x) { return std::isfinite(x); });
            if (revision_mismatch || invalid_transform) return cxx::unexpected(RenderError{kStorageContract});
        }
        if (order.empty()) return TransformApplied{input.sequence, 0};
        std::sort(order.begin(), order.end(), [&](auto a, auto b) {
            const auto ai = input.updates[a].instance.local.index, bi = input.updates[b].instance.local.index;
            return ai < bi || (ai == bi && a > b);
        });
        order.erase(std::unique(order.begin(), order.end(), [&](auto a, auto b) {
            return input.updates[a].instance == input.updates[b].instance;
        }), order.end());
        auto selected = writable();
        if (!selected) return cxx::unexpected(selected.error());
        auto& backing = **selected;
        // Dirty indices are unique and bounded by allocated slots. No live-record walk on sparse updates.
        for (auto index : backing.pending)
        {
            auto result = session_.write(*session_.borrow(backing.buffer), std::uint64_t(index) * sizeof(TransformMatrix),
                std::as_bytes(std::span(&shadow_[index], 1)));
            if (!result) return cxx::unexpected(result.error());
            ++written_records_;
        }
        for (auto position : order)
        {
            const auto& value = input.updates[position];
            if (auto result = write(backing, value.instance.local.index, value.transform); !result)
                return cxx::unexpected(result.error());
        }
        for (auto position : order)
        {
            const auto& value = input.updates[position];
            auto& entry = *entries_.find(value.instance.local);
            entry.description.transform = value.transform;
            ++entry.revision;
            shadow_[value.instance.local.index] = value.transform;
            for (auto& bank : banks_) bank->mark(value.instance.local.index);
        }
        for (auto index : backing.pending) backing.dirty[index] = false;
        backing.pending.clear();
        publish(backing);
        return TransformApplied{input.sequence, static_cast<std::uint32_t>(order.size())};
    }

    RenderResult<InstanceInformation> InstanceStorage::query(InstanceId id) const noexcept
    {
        const auto* entry = id.storage == id_ ? entries_.find(id.local) : nullptr;
        if (!entry || !entry->revision) return cxx::unexpected(RenderError{kStorageIdentity});
        return InstanceInformation{{id, entry->revision}, entry->description};
    }

    RenderResult<void> InstanceStorage::release(InstancePublished input) noexcept
    {
        const auto* entry = input.instance.storage == id_ ? entries_.find(input.instance.local) : nullptr;
        if (!entry) return cxx::unexpected(RenderError{kStorageIdentity});
        if (input.revision != entry->revision) return cxx::unexpected(RenderError{kStorageContract});
        // Draw membership is a cold selection. Existing compiled selections must be withdrawn first.
        if (!uses_.empty()) return cxx::unexpected(RenderError{kBusy});
        entries_.erase(input.instance.local);
        return {};
    }

    RenderResult<void> InstanceStorage::prepare(const NativeResourcePlan& plan, const InstanceBinding& input,
        PreparedStorageBinding& binding) noexcept
    {
        const auto* description = std::get_if<GraphBufferDesc>(&plan.description);
        const bool invalid_range = !description || !input.record_count || input.record_count > shadow_.size() ||
            description->size != std::uint64_t(input.record_count) * sizeof(TransformMatrix);
        if (invalid_range) return cxx::unexpected(RenderError{kStorageMismatch});
        auto use = std::make_unique<Use>(*this, binding);
        use->update(*current_);
        uses_.push_back(use.get());
        binding.reference = {use.release(), [](void* value) noexcept { delete static_cast<Use*>(value); }};
        return {};
    }

    StorageFactory instanceStorageFactory() noexcept
    {
        static const render::InstanceStorage::Ops ops{
            [](const void* value, InstanceId id) noexcept { return static_cast<const InstanceStorage*>(value)->query(id); }
        };
        static const StorageBindingOps binding{
            [](void* value, const NativeResourcePlan& plan, const void* argument, PreparedStorageBinding& prepared) noexcept {
                return static_cast<InstanceStorage*>(value)->prepare(
                    plan, *static_cast<const InstanceBinding*>(argument), prepared
                );
            }
        };
        static const auto create = +[](DeviceSession& session, StorageId id, const InstanceStorageConfig& config) noexcept
            -> RenderResult<std::unique_ptr<InstanceStorage>> {
            return std::make_unique<InstanceStorage>(session, id, config);
        };
        static const auto compatible = +[](const InstanceStorage& value, const InstanceStorageConfig& config) noexcept {
            return value.compatibleConfig(config);
        };
        return makeStorageFactory<render::InstanceStorage, InstanceStorage, DeviceSession, create, compatible>(ops, &binding);
    }
}
