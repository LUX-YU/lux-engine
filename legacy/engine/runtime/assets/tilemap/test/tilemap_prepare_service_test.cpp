#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>
#include <lux/engine/ecs/tilemap/TilemapChunkCodec.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>
#include <lux/engine/runtime/execution/testing/AsyncCloseTestDriver.hpp>
#include <lux/engine/runtime/assets/tilemap/TilemapPrepareService.hpp>

#include <stdexec/execution.hpp>
#include <exec/start_detached.hpp>

#include <atomic>
#include <cassert>
#include <optional>
#include <string>
#include <utility>

namespace
{
    template<class Predicate>
    void drive(
        lux::exec::AsyncRuntime& runtime,
        Predicate&& predicate)
    {
        lux::exec::testing::CloseEpoch progress{runtime};
        progress.drive(std::forward<Predicate>(predicate));
    }
}

int main()
{
    namespace tilemap_prepare = lux::runtime::assets::tilemap;
    namespace tilemap_streaming = lux::ecs::tilemap::streaming;

    lux::exec::AsyncRuntimeBuilder builder;
    auto service = tilemap_prepare::TilemapPrepareService::addTo(builder);
    assert(service);
    auto plan = std::move(builder).compile();
    assert(plan);
    lux::exec::AsyncRuntime runtime{
        std::move(*plan),
        lux::exec::AsyncRuntimeConfig{
            .blocking_io_threads = 1u,
            .background_cpu_concurrency = 1u}};
    lux::exec::AsyncScope scope{runtime};

    lux::tilemap::TilemapChunkBlobV1 source;
    source.tiles.assign(lux::tilemap::kTilemapChunkTileCount, 19u);
    auto encoded = lux::tilemap::encodeTilemapChunkBlob(source);
    assert(encoded);
    const auto type = lux::ecs::scene_format::ContentTypeId{
        std::string{lux::tilemap::kTilemapChunkContentTypeName}};
    const auto digest = lux::ecs::scene_format::makeContentBlobId(
        type,
        lux::tilemap::kTilemapChunkSchemaVersion,
        *encoded).digest;

    std::optional<lux::async::OperationOutcome<
        tilemap_streaming::PrepareTilemapChunk>> prepared;
    std::atomic<bool> done{false};
    auto pipeline = lux::exec::execute(
            service->client().operation(),
            tilemap_streaming::PrepareTilemapChunk{
                lux::cxx::SharedBytes<>::copyOf(*encoded),
                {-17, 23},
                digest,
                7u},
            lux::async::SubmitOptions{
                .accounted_bytes = encoded->size() +
                    lux::tilemap::kTilemapChunkTileCount *
                        sizeof(std::uint16_t)})
        | stdexec::continues_on(
              lux::exec::mainThreadScheduler(runtime))
        | stdexec::then(
              [&](auto outcome) noexcept
              {
                  prepared.emplace(std::move(outcome));
                  done.store(true, std::memory_order_release);
              });
    assert(lux::exec::spawn(scope, std::move(pipeline)));
    drive(runtime, [&]() noexcept
    {
        return done.load(std::memory_order_acquire);
    });
    assert(prepared && *prepared);
    assert((*prepared)->request_generation == 7u);
    assert(((*prepared)->load.coordinate ==
        lux::ecs::TileChunkCoord{-17, 23}));
    assert((*prepared)->load.base_digest == digest);
    assert((*prepared)->load.tiles == source.tiles);

    std::optional<lux::async::OperationOutcome<
        tilemap_streaming::PrepareTilemapChunk>> rejected;
    done.store(false, std::memory_order_release);
    auto backpressure = lux::exec::execute(
            service->client().operation(),
            tilemap_streaming::PrepareTilemapChunk{},
            lux::async::SubmitOptions{
                .accounted_bytes =
                    tilemap_prepare::kTilemapPrepareByteBudget + 1u})
        | stdexec::continues_on(
              lux::exec::mainThreadScheduler(runtime))
        | stdexec::then(
              [&](auto outcome) noexcept
              {
                  rejected.emplace(std::move(outcome));
                  done.store(true, std::memory_order_release);
              });
    assert(lux::exec::spawn(scope, std::move(backpressure)));
    drive(runtime, [&]() noexcept
    {
        return done.load(std::memory_order_acquire);
    });
    assert(rejected && !*rejected && rejected->error().isRuntime());
    assert(rejected->error().runtimeError() ==
        lux::async::ESubmitError::BYTE_BUDGET_EXHAUSTED);

    lux::exec::testing::CloseEpoch close_progress{runtime};
    std::atomic<bool> scope_closed{false};
    auto close = scope.closeAsync()
        | stdexec::then(
              [&]() noexcept
              {
                  scope_closed.store(true, std::memory_order_release);
                  close_progress.notify();
              });
    ::experimental::execution::start_detached(std::move(close));
    close_progress.drive([&]() noexcept
    {
        return scope_closed.load(std::memory_order_acquire);
    });
    service->close();
    assert(!service->client());
    const auto closed = lux::exec::testing::closeRuntime(runtime);
    assert(closed.clean());
    return 0;
}
