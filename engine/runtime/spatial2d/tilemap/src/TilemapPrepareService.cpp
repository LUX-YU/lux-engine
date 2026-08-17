#include <lux/engine/runtime/spatial2d/tilemap/TilemapPrepareService.hpp>

#include <lux/engine/resource/tilemap/TilemapChunk.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>

#include <stdexec/execution.hpp>

#include <atomic>
#include <utility>

namespace lux::runtime::spatial2d
{
    namespace ex = stdexec;

    namespace
    {
        using PrepareResult = lux::cxx::expected<PreparedTilemapChunk, ETilemapPrepareError>;

        struct PendingPrepare final
        {
            explicit PendingPrepare(
                lux::exec::AsyncOperationCompletion<PrepareTilemapChunk>
                    value) noexcept
                : completion(std::move(value))
            {}

            void settle(PrepareResult result) noexcept
            {
                if (settled.exchange(true, std::memory_order_acq_rel))
                    return;
                if (result)
                    completion.complete(std::move(*result));
                else
                    completion.complete(lux::cxx::unexpected(
                        lux::exec::AsyncFailure<ETilemapPrepareError>::domain(
                            result.error())));
            }

            void stop() noexcept
            {
                if (settled.exchange(true, std::memory_order_acq_rel))
                    return;
                completion.failRuntime(
                    lux::exec::EAsyncSubmitError::STOPPING);
            }

            std::atomic<bool> settled{false};
            lux::exec::AsyncOperationCompletion<PrepareTilemapChunk>
                completion;
        };
    } // namespace

    TilemapPrepareClient::TilemapPrepareClient(
        std::weak_ptr<detail::TilemapPrepareControl> control,
        lux::exec::AsyncOperationClient<PrepareTilemapChunk> operation)
        noexcept
        : control_(std::move(control)), operation_(std::move(operation))
    {}

    TilemapPrepareClient::operator bool() const noexcept
    {
        const auto control = control_.lock();
        return control &&
            !control->closing.load(std::memory_order_acquire) &&
            static_cast<bool>(operation_);
    }

    lux::cxx::expected<
        TilemapPrepareService,
        lux::exec::AsyncAssemblyFailure>
    TilemapPrepareService::addTo(lux::exec::AsyncRuntimeBuilder& builder)
    {
        auto control = std::make_shared<detail::TilemapPrepareControl>();
        auto operation = builder.addOperation<PrepareTilemapChunk>(
            [control](
                PrepareTilemapChunk&& request,
                lux::exec::AsyncOperationContext& context,
                lux::exec::AsyncOperationCompletion<PrepareTilemapChunk>&&
                    completion) noexcept
            {
                auto pending = std::make_shared<PendingPrepare>(
                    std::move(completion));
                if (control->closing.load(std::memory_order_acquire))
                {
                    pending->settle(lux::cxx::unexpected(
                        ETilemapPrepareError::SERVICE_CLOSED));
                    return;
                }
                if (request.content.empty() || request.digest ==
                        lux::cxx::algorithm::Sha256Digest{} ||
                    request.request_generation == 0u)
                {
                    pending->settle(lux::cxx::unexpected(
                        ETilemapPrepareError::INVALID_REQUEST));
                    return;
                }

                auto work = ex::schedule(
                        lux::exec::backgroundCpuScheduler(
                            context.runtime()))
                    | ex::then(
                          [request = std::move(request)]() mutable noexcept
                              -> PrepareResult
                          {
                              auto decoded =
                                  lux::tilemap::decodeTilemapChunkBlob(
                                      request.content.view());
                              if (!decoded)
                              {
                                  return lux::cxx::unexpected(
                                      ETilemapPrepareError::DECODE_FAILED);
                              }
                              lux::ecs::TileChunkLoad load;
                              load.coordinate = request.coordinate;
                              load.tiles = std::move(decoded->tiles);
                              load.base_digest = request.digest;
                              load.active = false;
                              return PreparedTilemapChunk{
                                  std::move(load),
                                  request.request_generation};
                          })
                    | ex::continues_on(
                          lux::exec::mainThreadScheduler(
                              context.runtime()))
                    | ex::then(
                          [pending](PrepareResult result) noexcept
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
                kTilemapPrepareQueueCapacity,
                kTilemapPrepareByteBudget,
                kTilemapPrepareDrainBatch});
        if (!operation)
            return lux::cxx::unexpected(operation.error());
        return TilemapPrepareService{
            std::move(control), std::move(*operation)};
    }

    TilemapPrepareService::TilemapPrepareService(
        std::shared_ptr<detail::TilemapPrepareControl> control,
        lux::exec::AsyncOperationClient<PrepareTilemapChunk> operation)
        noexcept
        : control_(std::move(control)), operation_(std::move(operation))
    {}

    TilemapPrepareService::~TilemapPrepareService()
    {
        close();
    }

    TilemapPrepareClient TilemapPrepareService::client() const noexcept
    {
        return !closed_ && control_
            ? TilemapPrepareClient{control_, operation_}
            : TilemapPrepareClient{};
    }

    void TilemapPrepareService::close() noexcept
    {
        if (closed_)
            return;
        closed_ = true;
        if (control_)
            control_->closing.store(true, std::memory_order_release);
        operation_ = {};
    }
} // namespace lux::runtime::spatial2d
