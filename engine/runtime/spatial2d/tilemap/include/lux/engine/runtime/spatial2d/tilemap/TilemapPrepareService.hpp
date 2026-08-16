#pragma once
/**
 * @file TilemapPrepareService.hpp
 * @brief Bounded typed background decoding for cooked LXTC chunks.
 */

#include <lux/cxx/memory/SharedBytes.hpp>
#include <lux/engine/ecs/tilemap/TilemapTypes.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>
#include <lux/engine/runtime/spatial2d/tilemap/visibility.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace lux::runtime::spatial2d
{
    inline constexpr std::size_t kTilemapPrepareQueueCapacity = 32u;
    inline constexpr std::size_t kTilemapPrepareByteBudget =
        8u * 1024u * 1024u;
    inline constexpr std::size_t kTilemapPrepareDrainBatch = 8u;

    enum class ETilemapPrepareError : std::uint8_t
    {
        INVALID_REQUEST,
        DECODE_FAILED,
        SERVICE_CLOSED
    };

    struct PreparedTilemapChunk final
    {
        lux::ecs::TileChunkLoad load;
        std::uint64_t request_generation{0u};
    };

    struct PrepareTilemapChunk final
    {
        using Value = PreparedTilemapChunk;
        using Error = ETilemapPrepareError;

        lux::cxx::SharedBytes<> content;
        lux::ecs::TileChunkCoord coordinate;
        lux::cxx::algorithm::Sha256Digest digest;
        std::uint64_t request_generation{0u};
    };

    namespace detail
    {
        struct TilemapPrepareControl final
        {
            std::atomic<bool> closing{false};
        };
    } // namespace detail

    class LUX_ENGINE_RUNTIME_SPATIAL2D_TILEMAP_PUBLIC
        TilemapPrepareClient final
    {
    public:
        TilemapPrepareClient() noexcept = default;

        [[nodiscard]] const lux::exec::AsyncOperationClient<
            PrepareTilemapChunk>& operation() const noexcept
        {
            return operation_;
        }
        [[nodiscard]] explicit operator bool() const noexcept;

    private:
        friend class TilemapPrepareService;
        TilemapPrepareClient(
            std::weak_ptr<detail::TilemapPrepareControl> control,
            lux::exec::AsyncOperationClient<PrepareTilemapChunk> operation)
            noexcept;

        std::weak_ptr<detail::TilemapPrepareControl> control_;
        lux::exec::AsyncOperationClient<PrepareTilemapChunk> operation_;
    };

    class LUX_ENGINE_RUNTIME_SPATIAL2D_TILEMAP_PUBLIC
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

        [[nodiscard]] TilemapPrepareClient client() const noexcept;
        void close() noexcept;

    private:
        TilemapPrepareService(
            std::shared_ptr<detail::TilemapPrepareControl> control,
            lux::exec::AsyncOperationClient<PrepareTilemapChunk> operation)
            noexcept;

        std::shared_ptr<detail::TilemapPrepareControl> control_;
        lux::exec::AsyncOperationClient<PrepareTilemapChunk> operation_;
        bool closed_{false};
    };
} // namespace lux::runtime::spatial2d
