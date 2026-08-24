#include <lux/engine/runtime/assets/pixel/Infinite2DPixelPrepareService.hpp>

#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>
#include <lux/engine/runtime/assets/pixel/Infinite2DPixelContent.hpp>
#include <lux/engine/ecs/pixel/systems/PixelChunkPersistence.hpp>

#include <stdexec/execution.hpp>

#include <atomic>
#include <utility>

namespace lux::runtime::assets::pixel
{
    namespace ex = stdexec;
    using namespace lux::ecs::pixel::streaming;

    namespace
    {
        using PrepareResult = lux::cxx::expected<PreparedInfinite2DPixelChunk, EInfinite2DPixelPrepareError>;

        struct PendingPrepare final
        {
            explicit PendingPrepare(
                lux::exec::AsyncOperationCompletion<
                    PrepareInfinite2DPixelChunk> value) noexcept
                : completion(std::move(value))
            {}

            void settle(PrepareResult result) noexcept
            {
                if (settled.exchange(true, std::memory_order_acq_rel))
                    return;
                if (result)
                {
                    completion.complete(std::move(*result));
                    return;
                }
                completion.complete(lux::cxx::unexpected(
                    lux::async::OperationFailure<
                        EInfinite2DPixelPrepareError>::domain(
                            result.error())));
            }

            void stop() noexcept
            {
                if (settled.exchange(true, std::memory_order_acq_rel))
                    return;
                completion.failRuntime(
                    lux::async::ESubmitError::STOPPING);
            }

            std::atomic<bool> settled{false};
            lux::exec::AsyncOperationCompletion<
                PrepareInfinite2DPixelChunk> completion;
        };
    }

    lux::cxx::expected<
        Infinite2DPixelPrepareService,
        lux::exec::AsyncAssemblyFailure>
    Infinite2DPixelPrepareService::addTo(
        lux::exec::AsyncRuntimeBuilder& builder)
    {
        auto state = std::make_shared<lux::ecs::pixel::streaming::detail::
            Infinite2DPixelPrepareState>();
        auto operation = builder.addOperation<PrepareInfinite2DPixelChunk>(
            [state](
                PrepareInfinite2DPixelChunk&& request,
                lux::exec::AsyncOperationContext& context,
                lux::exec::AsyncOperationCompletion<
                    PrepareInfinite2DPixelChunk>&& completion) noexcept
            {
                auto pending = std::make_shared<PendingPrepare>(
                    std::move(completion));
                if (state->closing.load(std::memory_order_acquire))
                {
                    pending->settle(lux::cxx::unexpected(
                        EInfinite2DPixelPrepareError::SERVICE_CLOSED));
                    return;
                }
                if (request.content.empty() || !request.reference.valid() ||
                    request.request_generation == 0u)
                {
                    pending->settle(lux::cxx::unexpected(
                        EInfinite2DPixelPrepareError::INVALID_REQUEST));
                    return;
                }
                auto work = ex::schedule(
                        lux::exec::backgroundCpuScheduler(
                            context.runtime()))
                    | ex::then(
                          [request = std::move(request)]() mutable noexcept
                              -> PrepareResult
                          {
                              auto chunk = decodeInfinite2DPixelChunk(
                                  std::move(request.content),
                                  std::move(request.reference));
                              if (!chunk || chunk->coordinate !=
                                      request.expected_coordinate)
                              {
                                  return lux::cxx::unexpected(
                                      EInfinite2DPixelPrepareError::
                                          CONTENT_INVALID);
                              }
                              if (request.persistence)
                              {
                                  auto merged =
                                      lux::ecs::mergePixelChunkPersistence(
                                          *chunk,
                                          *request.persistence);
                                  if (!merged)
                                  {
                                      return lux::cxx::unexpected(
                                          EInfinite2DPixelPrepareError::
                                              CONTENT_INVALID);
                                  }
                              }
                              auto prepared = lux::ecs::preparePixelChunk(
                                  std::move(*chunk),
                                  std::move(request.preparation));
                              if (!prepared)
                              {
                                  return lux::cxx::unexpected(
                                      EInfinite2DPixelPrepareError::
                                          CONTENT_INVALID);
                              }
                              return PreparedInfinite2DPixelChunk{
                                  request.request_generation,
                                  std::move(*prepared)};
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
                kInfinite2DPixelPrepareQueueCapacity,
                kInfinite2DPixelPrepareByteBudget,
                kInfinite2DPixelPrepareDrainBatch});
        if (!operation)
            return lux::cxx::unexpected(operation.error());
        return Infinite2DPixelPrepareService{
            std::move(state), std::move(*operation)};
    }

    Infinite2DPixelPrepareService::Infinite2DPixelPrepareService(
        std::shared_ptr<lux::ecs::pixel::streaming::detail::
            Infinite2DPixelPrepareState> state,
        lux::async::OperationPort<PrepareInfinite2DPixelChunk>
            operation) noexcept
        : state_(std::move(state)), operation_(std::move(operation))
    {}

    Infinite2DPixelPrepareService::~Infinite2DPixelPrepareService()
    {
        close();
    }

    lux::ecs::pixel::streaming::Infinite2DPixelPrepareClient
    Infinite2DPixelPrepareService::client() const noexcept
    {
        return !closed_ && state_
            ? lux::ecs::pixel::streaming::Infinite2DPixelPrepareClient{
                  state_, operation_}
            : lux::ecs::pixel::streaming::Infinite2DPixelPrepareClient{};
    }

    void Infinite2DPixelPrepareService::close() noexcept
    {
        if (closed_)
            return;
        closed_ = true;
        if (state_)
            state_->closing.store(true, std::memory_order_release);
        operation_ = {};
    }
} // namespace lux::runtime::assets::pixel
