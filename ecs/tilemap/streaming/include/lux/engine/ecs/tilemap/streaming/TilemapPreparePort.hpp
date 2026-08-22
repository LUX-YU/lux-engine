#pragma once
/**
 * @file TilemapPreparePort.hpp
 * @brief Typed preparation capability consumed by Tilemap ECS streaming.
 */

#include <lux/cxx/memory/SharedBytes.hpp>
#include <lux/engine/core/async/OperationPort.hpp>
#include <lux/engine/ecs/tilemap/TilemapTypes.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

namespace lux::ecs::tilemap::streaming
{
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
        struct TilemapPrepareState final
        {
            std::atomic<bool> closing{false};
        };
    } // namespace detail

    /// Narrow value passed into the ECS System. Queue and scheduler ownership
    /// remain entirely behind the OperationPort endpoint.
    class TilemapPrepareClient final
    {
    public:
        TilemapPrepareClient() noexcept = default;

        TilemapPrepareClient(
            std::weak_ptr<detail::TilemapPrepareState> state,
            lux::async::OperationPort<PrepareTilemapChunk> operation)
            noexcept
            : state_(std::move(state)), operation_(std::move(operation))
        {}

        [[nodiscard]] const lux::async::OperationPort<
            PrepareTilemapChunk>& operation() const noexcept
        {
            return operation_;
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            const auto state = state_.lock();
            return state &&
                !state->closing.load(std::memory_order_acquire) &&
                static_cast<bool>(operation_);
        }

    private:
        std::weak_ptr<detail::TilemapPrepareState> state_;
        lux::async::OperationPort<PrepareTilemapChunk> operation_;
    };
} // namespace lux::ecs::tilemap::streaming
