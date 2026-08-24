#pragma once
/**
 * @file TilemapPrepareService.hpp
 * @brief Bounded typed background decoding for cooked LXTC chunks.
 */

#include <lux/engine/ecs/tilemap/streaming/TilemapPreparePort.hpp>
#include <lux/engine/runtime/assets/tilemap/visibility.h>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>

#include <cstddef>
#include <memory>

namespace lux::runtime::assets::tilemap
{
    inline constexpr std::size_t kTilemapPrepareQueueCapacity = 32u;
    inline constexpr std::size_t kTilemapPrepareByteBudget =
        8u * 1024u * 1024u;
    inline constexpr std::size_t kTilemapPrepareDrainBatch = 8u;

    class LUX_ENGINE_RUNTIME_TILEMAP_PREPARE_PUBLIC
        TilemapPrepareService final
    {
    public:
        [[nodiscard]] static lux::cxx::expected<
            TilemapPrepareService,
            lux::exec::AsyncAssemblyFailure>
        addTo(lux::exec::AsyncRuntimeBuilder& builder);

        TilemapPrepareService(const TilemapPrepareService&) = delete;
        TilemapPrepareService& operator=(const TilemapPrepareService&) =
            delete;
        TilemapPrepareService(TilemapPrepareService&&) noexcept = default;
        TilemapPrepareService& operator=(TilemapPrepareService&&) noexcept =
            default;
        ~TilemapPrepareService();

        [[nodiscard]] lux::ecs::tilemap::streaming::TilemapPrepareClient
        client() const noexcept;
        void close() noexcept;

    private:
        TilemapPrepareService(
            std::shared_ptr<
                lux::ecs::tilemap::streaming::detail::TilemapPrepareState>
                state,
            lux::async::OperationPort<
                lux::ecs::tilemap::streaming::PrepareTilemapChunk> operation)
            noexcept;

        std::shared_ptr<
            lux::ecs::tilemap::streaming::detail::TilemapPrepareState>
            state_;
        lux::async::OperationPort<
            lux::ecs::tilemap::streaming::PrepareTilemapChunk> operation_;
        bool closed_{false};
    };
} // namespace lux::runtime::assets::tilemap
