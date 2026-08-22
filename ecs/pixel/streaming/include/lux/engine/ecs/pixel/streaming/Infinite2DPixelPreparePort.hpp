#pragma once
/**
 * @file Infinite2DPixelPreparePort.hpp
 * @brief Typed preparation capability consumed by Pixel ECS streaming.
 */

#include <lux/cxx/memory/SharedBytes.hpp>
#include <lux/engine/core/async/OperationPort.hpp>
#include <lux/engine/ecs/pixel/systems/PixelFieldRuntime.hpp>
#include <lux/engine/ecs/scene_format/EntitySection.hpp>
#include <lux/engine/ecs/scene_format/PersistenceJournal.hpp>
#include <lux/engine/math/Grid.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace lux::ecs::pixel::streaming
{
    enum class EInfinite2DPixelPrepareError : std::uint8_t
    {
        INVALID_REQUEST,
        CONTENT_INVALID,
        SERVICE_CLOSED
    };

    struct PreparedInfinite2DPixelChunk final
    {
        std::uint64_t request_generation{0u};
        lux::ecs::PreparedPixelChunk chunk;
    };

    struct PrepareInfinite2DPixelChunk final
    {
        using Value = PreparedInfinite2DPixelChunk;
        using Error = EInfinite2DPixelPrepareError;

        lux::cxx::SharedBytes<> content;
        lux::ecs::scene_format::ContentBlobRef reference;
        lux::math::GridCoord2i64 expected_coordinate;
        lux::ecs::PixelChunkPreparationContext preparation;
        std::optional<lux::ecs::scene_format::PersistenceJournalRecord>
            persistence;
        std::uint64_t request_generation{0u};
    };

    namespace detail
    {
        struct Infinite2DPixelPrepareState final
        {
            std::atomic<bool> closing{false};
        };
    } // namespace detail

    class Infinite2DPixelPrepareClient final
    {
    public:
        Infinite2DPixelPrepareClient() noexcept = default;

        Infinite2DPixelPrepareClient(
            std::weak_ptr<detail::Infinite2DPixelPrepareState> state,
            lux::async::OperationPort<PrepareInfinite2DPixelChunk> operation)
            noexcept
            : state_(std::move(state)), operation_(std::move(operation))
        {}

        [[nodiscard]] const lux::async::OperationPort<
            PrepareInfinite2DPixelChunk>& operation() const noexcept
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
        std::weak_ptr<detail::Infinite2DPixelPrepareState> state_;
        lux::async::OperationPort<PrepareInfinite2DPixelChunk> operation_;
    };
} // namespace lux::ecs::pixel::streaming
