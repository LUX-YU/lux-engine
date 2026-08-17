#include <lux/engine/runtime/entity_scene/EntitySectionService.hpp>

#include <lux/engine/resource/entity_scene/EntitySceneCodec.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>

#include <stdexec/execution.hpp>
#include <zstd.h>

#include <atomic>
#include <limits>
#include <span>
#include <utility>

namespace lux::runtime::entity_scene
{
    namespace ex = stdexec;

    namespace
    {
        template <typename T>
        using EntitySectionLoadExp = lux::cxx::expected<T, EEntitySectionLoadError>;

        using LoadResult = EntitySectionLoadExp<EntitySectionLoadResult>;
        using OpenResult = EntitySectionLoadExp<lux::asset::AssetBlob>;

        struct PendingLoad final
        {
            explicit PendingLoad(
                lux::exec::AsyncOperationCompletion<LoadEntitySection> value)
                noexcept
                : completion(std::move(value))
            {}

            void settle(LoadResult result) noexcept
            {
                if (settled.exchange(true, std::memory_order_acq_rel))
                    return;
                if (result)
                {
                    completion.complete(std::move(*result));
                    return;
                }
                completion.complete(lux::cxx::unexpected(
                    lux::exec::AsyncFailure<EEntitySectionLoadError>::domain(
                        result.error())));
            }

            void stop() noexcept
            {
                if (settled.exchange(true, std::memory_order_acq_rel))
                    return;
                completion.failRuntime(lux::exec::EAsyncSubmitError::STOPPING);
            }

            std::atomic<bool> settled{false};
            lux::exec::AsyncOperationCompletion<LoadEntitySection> completion;
        };

        [[nodiscard]] EEntitySectionLoadError mapOpenError(
            lux::asset::EAssetError error) noexcept
        {
            switch (error)
            {
            case lux::asset::EAssetError::ASSET_NOT_EXIST:
            case lux::asset::EAssetError::FILE_NOT_EXIST:
            case lux::asset::EAssetError::TARGET_NOT_EXIST:
                return EEntitySectionLoadError::SECTION_NOT_FOUND;
            default:
                return EEntitySectionLoadError::IO_FAILED;
            }
        }

        [[nodiscard]] OpenResult openStored(
            const std::shared_ptr<const lux::asset::AssetVfs>& vfs,
            const std::string& path) noexcept
        {
            const auto id = vfs->resolve(path);
            if (id.is_nil())
            {
                return lux::cxx::unexpected(
                    EEntitySectionLoadError::SECTION_NOT_FOUND);
            }
            auto blob = vfs->open(id);
            if (!blob)
                return lux::cxx::unexpected(mapOpenError(blob.error()));
            return std::move(*blob);
        }

        [[nodiscard]] LoadResult decodeLoaded(
            lux::asset::AssetBlob blob,
            lux::entity_scene::EntitySectionRecord record,
            std::uint64_t request_generation) noexcept
        {
            if (!blob || blob.bytes.size() != record.encoded_bytes)
            {
                return lux::cxx::unexpected(
                    EEntitySectionLoadError::ENCODED_SIZE_MISMATCH);
            }

                lux::cxx::SharedBytes<> decoded_bytes;
                if (record.compression ==
                    lux::entity_scene::EEntitySectionCompression::NONE)
                {
                    decoded_bytes = std::move(blob.bytes);
                }
                else if (record.compression ==
                    lux::entity_scene::EEntitySectionCompression::ZSTD)
                {
                    if (record.decoded_bytes == 0u ||
                        record.decoded_bytes >
                            std::numeric_limits<std::size_t>::max())
                    {
                        return lux::cxx::unexpected(
                            EEntitySectionLoadError::DECODED_SIZE_MISMATCH);
                    }
                    const auto decoded_size = static_cast<std::size_t>(
                        record.decoded_bytes);
                    auto owner = std::shared_ptr<std::byte[]>{
                        new std::byte[decoded_size]};
                    const auto actual = ZSTD_decompress(
                        owner.get(),
                        decoded_size,
                        blob.bytes.data(),
                        blob.bytes.size());
                    if (ZSTD_isError(actual))
                    {
                        return lux::cxx::unexpected(
                            EEntitySectionLoadError::DECOMPRESSION_FAILED);
                    }
                    if (actual != decoded_size)
                    {
                        return lux::cxx::unexpected(
                            EEntitySectionLoadError::DECODED_SIZE_MISMATCH);
                    }
                    auto shared_owner =
                        std::shared_ptr<const std::byte[]>{std::move(owner)};
                    auto owned = lux::cxx::SharedBytes<>::fromOwner(
                        shared_owner,
                        std::span<const std::byte>{
                            shared_owner.get(), decoded_size});
                    if (owned.empty())
                    {
                        return lux::cxx::unexpected(
                            EEntitySectionLoadError::DECOMPRESSION_FAILED);
                    }
                    decoded_bytes = std::move(owned);
                }
                else
                {
                    return lux::cxx::unexpected(
                        EEntitySectionLoadError::DECOMPRESSION_FAILED);
                }

                if (decoded_bytes.size() != record.decoded_bytes)
                {
                    return lux::cxx::unexpected(
                        EEntitySectionLoadError::DECODED_SIZE_MISMATCH);
                }
                if (lux::entity_scene::entitySceneContentDigest(
                        decoded_bytes.view()) != record.content_digest)
                {
                    return lux::cxx::unexpected(
                        EEntitySectionLoadError::DIGEST_MISMATCH);
                }

                EntityBatchDecoder decoder;
                auto decoded = decoder.decode(
                    std::move(decoded_bytes), request_generation);
                if (!decoded)
                {
                    return lux::cxx::unexpected(
                        EEntitySectionLoadError::DECODE_FAILED);
                }
                if (decoded->section() != record.id ||
                    decoded->entityCount() != record.entity_count)
                {
                    return lux::cxx::unexpected(
                        EEntitySectionLoadError::RECORD_MISMATCH);
                }
            return EntitySectionLoadResult{
                request_generation, std::move(*decoded)};
        }
        [[nodiscard]] EEntitySectionLoadError mapGeneratorError(
            EEntitySectionGeneratorError error) noexcept
        {
            return error == EEntitySectionGeneratorError::NOT_FOUND
                ? EEntitySectionLoadError::GENERATOR_NOT_FOUND
                : EEntitySectionLoadError::GENERATION_FAILED;
        }

        [[nodiscard]] LoadResult generateLoaded(
            const std::shared_ptr<const EntitySectionGeneratorCatalog>&
                generators,
            lux::entity_scene::EntitySectionRecord record,
            std::uint64_t request_generation) noexcept
        {
            if (!generators)
            {
                return lux::cxx::unexpected(
                    EEntitySectionLoadError::GENERATOR_NOT_FOUND);
            }
            auto image = generators->generate(
                GeneratedEntitySectionRequest{record});
            if (!image)
            {
                return lux::cxx::unexpected(
                    mapGeneratorError(image.error().error));
            }
            auto encoded = lux::entity_scene::encodeEntitySectionImage(*image);
            if (!encoded)
            {
                return lux::cxx::unexpected(
                    EEntitySectionLoadError::ENCODE_FAILED);
            }
            auto bytes = lux::cxx::SharedBytes<>::copyOf(
                std::span<const std::byte>{
                    encoded->data(), encoded->size()});
            return decodeLoaded(
                lux::asset::AssetBlob::fromShared(std::move(bytes)),
                std::move(record),
                request_generation);
        }
    }

    EntitySectionLoadClient::EntitySectionLoadClient(
        std::weak_ptr<detail::EntitySectionOperationControl> control,
        lux::exec::AsyncOperationClient<LoadEntitySection> operation) noexcept
        : control_(std::move(control)), operation_(std::move(operation))
    {}

    LoadEntitySection EntitySectionLoadClient::loadOperation(
        std::shared_ptr<const lux::asset::AssetVfs> vfs,
        lux::entity_scene::EntitySectionRecord record,
        std::uint64_t request_generation) const noexcept
    {
        return {
            std::move(vfs), std::move(record), request_generation};
    }

    EntitySectionLoadClient::operator bool() const noexcept
    {
        const auto control = control_.lock();
        return control &&
            !control->closing.load(std::memory_order_acquire) &&
            static_cast<bool>(operation_);
    }

    bool EntitySectionLoadClient::supports(
        const lux::entity_scene::EntitySectionRecord& record) const noexcept
    {
        const auto control = control_.lock();
        if (!control || control->closing.load(std::memory_order_acquire) ||
            !operation_)
        {
            return false;
        }
        if (std::holds_alternative<lux::entity_scene::StoredSectionSource>(
                record.source))
        {
            return true;
        }
        const auto& generated =
            std::get<lux::entity_scene::GeneratedSectionSource>(
                record.source);
        return control->generators &&
            control->generators->contains(generated.generator);
    }

    lux::cxx::expected<
        EntitySectionService,
        lux::exec::AsyncAssemblyFailure>
    EntitySectionService::addTo(
        lux::exec::AsyncRuntimeBuilder& builder,
        std::shared_ptr<const EntitySectionGeneratorCatalog> generators)
    {
        auto control =
            std::make_shared<detail::EntitySectionOperationControl>();
        control->generators = std::move(generators);
        auto operation = builder.addOperation<LoadEntitySection>(
            [control](
                LoadEntitySection&& request,
                lux::exec::AsyncOperationContext& context,
                lux::exec::AsyncOperationCompletion<LoadEntitySection>&&
                    completion) noexcept
            {
                auto pending = std::make_shared<PendingLoad>(
                    std::move(completion));
                if (control->closing.load(std::memory_order_acquire))
                {
                    pending->settle(lux::cxx::unexpected(
                        EEntitySectionLoadError::SERVICE_CLOSED));
                    return;
                }
                if (request.request_generation == 0u ||
                    !lux::entity_scene::validateEntitySectionRecord(
                        request.record))
                {
                    pending->settle(lux::cxx::unexpected(
                        EEntitySectionLoadError::INVALID_REQUEST));
                    return;
                }
                const auto* stored = std::get_if<
                    lux::entity_scene::StoredSectionSource>(
                        &request.record.source);
                if (!stored)
                {
                    auto record = std::move(request.record);
                    const auto generation = request.request_generation;
                    auto generators = control->generators;
                    auto work = ex::schedule(
                            lux::exec::backgroundCpuScheduler(
                                context.runtime()))
                        | ex::then(
                              [generators = std::move(generators),
                               record = std::move(record),
                               generation]() mutable noexcept -> LoadResult
                              {
                                  return generateLoaded(
                                      generators,
                                      std::move(record),
                                      generation);
                              })
                        | ex::continues_on(
                              lux::exec::mainThreadScheduler(
                                  context.runtime()))
                        | ex::then(
                              [pending](LoadResult result) noexcept
                              {
                                  pending->settle(std::move(result));
                              })
                        | ex::upon_stopped(
                              [pending]() noexcept { pending->stop(); });
                    if (!lux::exec::spawn(context.scope(), std::move(work)))
                        pending->stop();
                    return;
                }
                if (!request.vfs)
                {
                    pending->settle(lux::cxx::unexpected(
                        EEntitySectionLoadError::INVALID_REQUEST));
                    return;
                }

                auto vfs = std::move(request.vfs);
                auto record = std::move(request.record);
                auto path = std::get<lux::entity_scene::StoredSectionSource>(
                    record.source).content_path;
                const auto generation = request.request_generation;
                auto work = ex::schedule(
                        lux::exec::blockingIoScheduler(context.runtime()))
                    | ex::then(
                          [vfs, path = std::move(path)]() noexcept
                          {
                              return openStored(vfs, path);
                          })
                    | ex::continues_on(
                          lux::exec::backgroundCpuScheduler(context.runtime()))
                    | ex::then(
                          [record = std::move(record), generation](
                              OpenResult opened) mutable noexcept -> LoadResult
                          {
                              if (!opened)
                                  return lux::cxx::unexpected(opened.error());
                              return decodeLoaded(
                                  std::move(*opened),
                                  std::move(record),
                                  generation);
                          })
                    | ex::continues_on(
                          lux::exec::mainThreadScheduler(context.runtime()))
                    | ex::then(
                          [pending](LoadResult result) noexcept
                          {
                              pending->settle(std::move(result));
                          })
                    | ex::upon_stopped(
                          [pending]() noexcept { pending->stop(); });
                if (!lux::exec::spawn(context.scope(), std::move(work)))
                    pending->stop();
            },
            {},
            lux::exec::AsyncOperationQueueConfig{
                .capacity = kEntitySectionLoadQueueCapacity,
                .byte_budget = kEntitySectionLoadByteBudget,
                .drain_batch = kEntitySectionLoadDrainBatch});
        if (!operation)
            return lux::cxx::unexpected(std::move(operation.error()));
        return EntitySectionService{control, std::move(*operation)};
    }
    EntitySectionService::EntitySectionService(
        std::shared_ptr<detail::EntitySectionOperationControl> control,
        lux::exec::AsyncOperationClient<LoadEntitySection> operation) noexcept
        : control_(std::move(control)), operation_(std::move(operation))
    {}

    EntitySectionService::EntitySectionService(
        EntitySectionService&& other) noexcept
        : control_(std::move(other.control_)),
          operation_(std::move(other.operation_)),
          closed_(std::exchange(other.closed_, true))
    {}

    EntitySectionService& EntitySectionService::operator=(
        EntitySectionService&& other) noexcept
    {
        if (this == &other)
            return *this;
        close();
        control_ = std::move(other.control_);
        operation_ = std::move(other.operation_);
        closed_ = std::exchange(other.closed_, true);
        return *this;
    }

    EntitySectionService::~EntitySectionService()
    {
        close();
    }

    EntitySectionLoadClient EntitySectionService::loadClient() const noexcept
    {
        return closed_ || !control_
            ? EntitySectionLoadClient{}
            : EntitySectionLoadClient{control_, operation_};
    }

    void EntitySectionService::close() noexcept
    {
        if (closed_)
            return;
        if (control_)
            control_->closing.store(true, std::memory_order_release);
        operation_ = {};
        closed_ = true;
    }
}
