#pragma once
/**
 * @file Infinite2DPixelPrepareService.hpp
 * @brief Bounded background expansion for compact Pixel chunk content.
 */

#include <lux/cxx/memory/SharedBytes.hpp>
#include <lux/engine/ecs/pixel/systems/PixelFieldRuntime.hpp>
#include <lux/engine/ecs/scene_format/PersistenceJournal.hpp>
#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>
#include <lux/engine/math/Grid.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>
#include <lux/engine/runtime/spatial2d/infinite/pixel_visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace lux::runtime::spatial2d
{
    inline constexpr std::size_t kInfinite2DPixelPrepareQueueCapacity = 32u;
    inline constexpr std::size_t kInfinite2DPixelPrepareByteBudget =
        16u * 1024u * 1024u;
    inline constexpr std::size_t kInfinite2DPixelPrepareDrainBatch = 8u;

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
        struct Infinite2DPixelPrepareControl final
        {
            std::atomic<bool> closing{false};
        };
    }

    class LUX_ENGINE_RUNTIME_INFINITE2D_PIXEL_PUBLIC
    Infinite2DPixelPrepareClient final
    {
    public:
        Infinite2DPixelPrepareClient() noexcept = default;

        [[nodiscard]] const lux::async::OperationPort<
            PrepareInfinite2DPixelChunk>& operation() const noexcept
        {
            return operation_;
        }

        [[nodiscard]] explicit operator bool() const noexcept;

    private:
        friend class Infinite2DPixelPrepareService;
        Infinite2DPixelPrepareClient(
            std::weak_ptr<detail::Infinite2DPixelPrepareControl> control,
            lux::async::OperationPort<PrepareInfinite2DPixelChunk>
                operation) noexcept;

        std::weak_ptr<detail::Infinite2DPixelPrepareControl> control_;
        lux::async::OperationPort<PrepareInfinite2DPixelChunk>
            operation_;
    };

    class LUX_ENGINE_RUNTIME_INFINITE2D_PIXEL_PUBLIC
    Infinite2DPixelPrepareService final
    {
    public:
        [[nodiscard]] static lux::cxx::expected<
            Infinite2DPixelPrepareService,
            lux::exec::AsyncAssemblyFailure>
        addTo(lux::exec::AsyncRuntimeBuilder& builder);

        Infinite2DPixelPrepareService(
            const Infinite2DPixelPrepareService&) = delete;
        Infinite2DPixelPrepareService& operator=(
            const Infinite2DPixelPrepareService&) = delete;
        Infinite2DPixelPrepareService(
            Infinite2DPixelPrepareService&&) noexcept = default;
        Infinite2DPixelPrepareService& operator=(
            Infinite2DPixelPrepareService&&) noexcept = default;
        ~Infinite2DPixelPrepareService();

        [[nodiscard]] Infinite2DPixelPrepareClient client() const noexcept;
        void close() noexcept;

    private:
        Infinite2DPixelPrepareService(
            std::shared_ptr<detail::Infinite2DPixelPrepareControl> control,
            lux::async::OperationPort<PrepareInfinite2DPixelChunk>
                operation) noexcept;

        std::shared_ptr<detail::Infinite2DPixelPrepareControl> control_;
        lux::async::OperationPort<PrepareInfinite2DPixelChunk>
            operation_;
        bool closed_{false};
    };
}
