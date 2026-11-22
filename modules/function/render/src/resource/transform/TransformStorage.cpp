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
        : session_(context.device), progress_(context.retirement_progress_), frame_count_(context.frame_count),
          paged_(session_.usesDeviceAddresses()), records_(id), completed_(session_.makeFrontiers()),
          resources_(context.resources)
    {
    }

    RenderResult<std::vector<TransformId>> TransformStorage::create(std::span<const TransformMatrix> matrices) noexcept
    {
        if (matrices.empty()) return std::vector<TransformId>{};
        collectRetired();
        if (revision_ == UINT64_MAX || !records_.canInsert(matrices.size()))
            return cxx::unexpected(RenderError{kStorageCapacity});
        if (!std::ranges::all_of(matrices, isFiniteTransform)) return cxx::unexpected(RenderError{kStorageContract});
        std::vector<TransformId> identities;
        identities.reserve(matrices.size());
        for (const auto& matrix : matrices)
        {
            const auto id = records_.insert(matrix);
            markDirty(extension_detail::ResourceIdentity::local(id).index);
            identities.push_back(id);
        }
        ++revision_;
        return identities;
    }

    RenderResult<void> TransformStorage::update(std::span<const TransformRecordUpdate> updates) noexcept
    {
        if (updates.empty()) return {};
        if (revision_ == UINT64_MAX) return cxx::unexpected(RenderError{kStorageCapacity});
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
        ++revision_;
        return {};
    }

    RenderResult<void> TransformStorage::release(TransformId id) noexcept
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
