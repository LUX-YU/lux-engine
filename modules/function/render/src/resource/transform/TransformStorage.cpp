#include <lux/engine/render/resource/transform/TransformStorage.hpp>

#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/extensions/resource/StorageMeta.hpp>
#include <lux/engine/render/runtime/FrameProgress.hpp>
#include <lux/engine/render/resources/transform/Resource.hpp>
#include <lux/engine/render/resources/transform/Binding.hpp>
#include <utility>

namespace lux::render::detail
{
    namespace
    {
        void createTransforms(
            TransformStorage& storage, const TransformCommands::Create::Request& input,
            TransformCommands::Create::Response& reply
        ) noexcept
        {
            auto result = storage.create(input.matrices);
            reply = result ? TransformCommands::Create::Response{{}, std::move(*result)}
                           : TransformCommands::Create::Response{result.error()};
        }

        RenderResult<void>
        updateTransforms(TransformStorage& storage, std::span<const TransformRecordUpdate> values) noexcept
        {
            return storage.update(values);
        }

        void updateTransformsCommand(
            TransformStorage& storage, const TransformCommands::Update::Request& input, AckResponse& reply
        ) noexcept
        {
            const auto result = updateTransforms(storage, input.values);
            reply.error = result ? RenderError{} : result.error();
        }

        void releaseTransform(TransformStorage& storage, const TransformId& id, AckResponse& reply) noexcept
        {
            const auto result = storage.release(id);
            reply.error = result ? RenderError{} : result.error();
        }

        RenderResult<std::unique_ptr<TransformStorage>>
        createTransformStorage(ResourceCreateContext& context, ResourceInstanceId id) noexcept
        {
            const bool invalid_replicas = context.frame_count == 0 || context.frame_count > 3;
            if (!context.uploads || invalid_replicas)
                return cxx::unexpected(RenderError{kDeviceArgument});
            return std::make_unique<TransformStorage>(context, id);
        }
    } // namespace

    TransformStorage::TransformStorage(ResourceCreateContext& context, ResourceInstanceId id) noexcept
        : id_(id), session_(context.device), progress_(context.retirement_progress_), frame_count_(context.frame_count),
          paged_(session_.usesDeviceAddresses()), completed_(session_.makeFrontiers()), resources_(context.resources)
    {
    }

    const TransformStorage::Record* TransformStorage::find(TransformId id) const noexcept
    {
        using Identity = extension_detail::ResourceIdentity;
        return Identity::parent(id) == id_ ? records_.find(Identity::local(id)) : nullptr;
    }

    TransformStorage::Record* TransformStorage::find(TransformId id) noexcept
    {
        using Identity = extension_detail::ResourceIdentity;
        return Identity::parent(id) == id_ ? records_.find(Identity::local(id)) : nullptr;
    }

    std::optional<std::uint32_t> TransformStorage::gpuSlot(TransformId id) const noexcept
    {
        if (const auto* record = find(id)) return record->gpu_slot;
        return std::nullopt;
    }

    void TransformStorage::retainInstance(TransformId id) noexcept
    {
        auto* record = find(id);
        if (!record || record->references == UINT32_MAX) std::terminate();
        ++record->references;
    }

    void TransformStorage::releaseInstance(TransformId id) noexcept
    {
        auto* record = find(id);
        if (!record || !record->references) std::terminate();
        --record->references;
    }

    bool TransformStorage::canCreate(std::size_t count) const noexcept
    {
        const auto growth = count > free_slots_.size() ? count - free_slots_.size() : 0;
        return count <= UINT32_MAX - records_.size() && growth <= UINT32_MAX - matrices_.size();
    }

    TransformId TransformStorage::insert(const TransformMatrix& matrix) noexcept
    {
        std::uint32_t slot;
        if (free_slots_.empty())
        {
            slot = static_cast<std::uint32_t>(matrices_.size());
            matrices_.push_back(matrix);
        }
        else
        {
            slot = free_slots_.back();
            free_slots_.pop_back();
            matrices_[slot] = matrix;
        }
        markDirty(slot);
        return extension_detail::ResourceIdentity::make<TransformId>(id_, records_.emplace(Record{slot}));
    }

    RenderResult<std::vector<TransformId>> TransformStorage::create(std::span<const TransformMatrix> matrices) noexcept
    {
        if (matrices.empty()) return std::vector<TransformId>{};
        collectRetired();
        if (revision_ == UINT64_MAX || !canCreate(matrices.size()))
            return cxx::unexpected(RenderError{kStorageCapacity});
        if (!std::ranges::all_of(matrices, isFiniteTransform)) return cxx::unexpected(RenderError{kStorageContract});
        std::vector<TransformId> identities;
        identities.reserve(matrices.size());
        for (const auto& matrix : matrices) identities.push_back(insert(matrix));
        ++revision_;
        return identities;
    }

    RenderResult<void> TransformStorage::update(std::span<const TransformRecordUpdate> updates) noexcept
    {
        if (updates.empty()) return {};
        if (revision_ == UINT64_MAX) return cxx::unexpected(RenderError{kStorageCapacity});
        for (const auto& update : updates)
        {
            if (!find(update.transform)) return cxx::unexpected(RenderError{kStorageIdentity});
            if (!isFiniteTransform(update.matrix)) return cxx::unexpected(RenderError{kStorageContract});
        }
        for (const auto& update : updates)
        {
            const auto slot = find(update.transform)->gpu_slot;
            matrices_[slot] = update.matrix;
            markDirty(slot);
        }
        ++revision_;
        return {};
    }

    RenderResult<void> TransformStorage::release(TransformId id) noexcept
    {
        const auto* record = find(id);
        if (!record) return cxx::unexpected(RenderError{kStorageIdentity});
        if (record->references) return cxx::unexpected(RenderError{kDeviceBusy});
        retirement_.release(record->gpu_slot);
        records_.erase(extension_detail::ResourceIdentity::local(id));
        collectRetired();
        return {};
    }

    void TransformStorage::collectRetired() noexcept
    {
        if (retirement_.empty() || session_.terminalError().type) return;
        session_.readCompleted(completed_);
        retirement_.collect(progress_.active(), progress_.frontiers(), completed_, free_slots_);
    }

    void TransformStorage::markDirty(std::uint32_t slot) noexcept
    {
        for (std::uint32_t frame = 0; frame < frame_count_; ++frame)
        {
            auto& dirty = replicas_[frame].dirty;
            dirty.grow(matrices_.size());
            dirty.mark(slot);
        }
    }

    ResourceRegistration TransformStorage::factory() noexcept
    {
        static const auto frame_call =
            FrameDataCall::declare<TransformResource, TransformStorage, &updateTransforms>();
        static const std::array commands{
            CommandBinding::declare<TransformCommands::Create, &createTransforms>(),
            CommandBinding::declare<TransformCommands::Update, &updateTransformsCommand>(),
            CommandBinding::declare<TransformCommands::Release, &releaseTransform>()
        };
        static const StorageBindingOps binding{
            nullptr,
            [](void* value, ReplicaFrame frame, ResourceUploader& uploads) noexcept
            { return static_cast<TransformStorage*>(value)->prepareFrame(frame.slot, uploads); },
            [](const void* value) noexcept { return &static_cast<const TransformStorage*>(value)->revision_; },
            [](const void* value, std::uint32_t projection) noexcept -> RenderResult<StorageGraphImport>
            {
                const auto& owner = *static_cast<const TransformStorage*>(value);
                const auto expected = owner.paged_ ? ETransformGraphProjection::PAGE_ADDRESSES :
                    ETransformGraphProjection::RECORDS;
                if (projection != static_cast<std::uint32_t>(expected))
                    return cxx::unexpected(RenderError{kStorageMismatch});
                const auto bytes = owner.paged_ ?
                    kTransformInitialPageAddresses * sizeof(std::uint64_t) : sizeof(TransformMatrix);
                return StorageGraphImport{
                    GraphBufferDesc{bytes}, {GraphBufferRange{0, bytes}}, EGraphUse::STORAGE_READ,
                    EGraphUse::STORAGE_READ
                };
            },
            [](void* value, std::uint32_t projection, const VGraphResourceDesc& plan,
               std::span<ResourceInput> output) noexcept -> RenderResult<void>
            {
                auto& owner = *static_cast<TransformStorage*>(value);
                const auto expected = owner.paged_ ? ETransformGraphProjection::PAGE_ADDRESSES :
                    ETransformGraphProjection::RECORDS;
                if (projection != static_cast<std::uint32_t>(expected))
                    return cxx::unexpected(RenderError{kStorageMismatch});
                return owner.resolve(plan, output);
            },
            nullptr,
            [](void* value, const ReplicaSettlement& settlement) noexcept
            { static_cast<TransformStorage*>(value)->finishPreparation(settlement); }
        };
        return makeResourceRegistration<TransformResource, TransformStorage, createTransformStorage>(
            nullptr, &binding, commands, frame_call
        );
    }
} // namespace lux::render::detail
