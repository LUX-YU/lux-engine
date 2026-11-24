#include <lux/engine/render/resource/transform/TransformStorage.hpp>

#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/resource/StorageMeta.hpp>
#include <lux/engine/render/runtime/FrameProgress.hpp>
#include <lux/engine/render/resource/transform/Resource.hpp>
#include <lux/engine/render/resource/transform/Binding.hpp>

namespace lux::render::detail
{
    namespace
    {
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
        : session_(context.device), progress_(context.retirement_progress_), frame_count_(context.frame_count),
          paged_(session_.usesDeviceAddresses()), records_(id), completed_(session_.makeFrontiers()),
          resources_(context.resources)
    {
    }

    RenderResult<std::vector<TransformRef>> TransformStorage::create(std::span<const TransformMatrix> matrices) noexcept
    {
        if (matrices.empty()) return std::vector<TransformRef>{};
        collectRetired();
        if (!records_.canInsert(matrices.size()))
            return cxx::unexpected(RenderError{kStorageCapacity});
        if (!std::ranges::all_of(matrices, isFiniteTransform)) return cxx::unexpected(RenderError{kStorageContract});
        std::vector<TransformRef> identities;
        identities.reserve(matrices.size());
        for (const auto& matrix : matrices)
        {
            const auto id = records_.insert(matrix);
            markDirty(extension_detail::ResourceIdentity::local(id).index);
            identities.push_back(id);
        }
        return identities;
    }

    RenderResult<void> TransformStorage::update(std::span<const TransformRecordUpdate> updates) noexcept
    {
        if (updates.empty()) return {};
        for (const auto& update : updates)
        {
            if (!records_.contains(update.transform)) return cxx::unexpected(RenderError{kStorageIdentity});
            if (!isFiniteTransform(update.matrix)) return cxx::unexpected(RenderError{kStorageContract});
        }
        for (const auto& update : updates)
        {
            const auto slot = extension_detail::ResourceIdentity::local(update.transform).index;
            records_.writeValidated(slot, update.matrix);
            markDirty(slot);
        }
        return {};
    }

    RenderResult<void> TransformStorage::release(TransformRef id) noexcept
    {
        auto released = records_.release(id);
        if (!released) return released;
        collectRetired();
        return {};
    }

    void TransformStorage::collectRetired() noexcept
    {
        if (!records_.hasRetired() || session_.terminalError().type) return;
        session_.readCompleted(completed_);
        records_.collect(progress_.active(), progress_.frontiers(), completed_);
    }

    void TransformStorage::markDirty(std::uint32_t slot) noexcept
    {
        for (std::uint32_t frame = 0; frame < frame_count_; ++frame)
        {
            auto& dirty = replicas_[frame].dirty;
            dirty.grow(records_.matrices().size());
            dirty.mark(slot);
        }
    }

    ResourceRegistration TransformStorage::factory() noexcept
    {
        static const StorageBindingOps binding{
            .prepareFrame = [](void* value, ReplicaFrame frame, ResourceUploader& uploads) noexcept
            { return static_cast<TransformStorage*>(value)->prepareFrame(frame.slot, uploads); },
            .describeGraphImport = [](const void* value, std::uint32_t projection) noexcept -> RenderResult<ResourceProjection>
            {
                const auto& owner = *static_cast<const TransformStorage*>(value);
                const auto expected = transformGraphProjection(owner.paged_);
                if (projection != static_cast<std::uint32_t>(expected))
                    return cxx::unexpected(RenderError{kStorageMismatch});
                return TransformResource::projection(expected);
            },
            .resolveGraphImport = [](void* value, std::uint32_t projection, const VGraphResourceDesc& plan,
               std::span<ResourceInput> output) noexcept -> RenderResult<void>
            {
                auto& owner = *static_cast<TransformStorage*>(value);
                const auto expected = transformGraphProjection(owner.paged_);
                if (projection != static_cast<std::uint32_t>(expected))
                    return cxx::unexpected(RenderError{kStorageMismatch});
                return owner.resolve(plan, output);
            },
            .settleFrame = [](void* value, const ReplicaSettlement& settlement) noexcept
            { static_cast<TransformStorage*>(value)->finishPreparation(settlement); }
        };
        return makeResourceRegistration<TransformResource, TransformStorage, createTransformStorage, &TransformStorage::update>(&binding);
    }
} // namespace lux::render::detail
