#pragma once
/**
 * @file Infinite2DPixelPrepareService.hpp
 * @brief Bounded background expansion for compact Pixel chunk content.
 */

#include <lux/engine/ecs/pixel/streaming/Infinite2DPixelPreparePort.hpp>
#include <lux/engine/runtime/assets/pixel/visibility.h>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <memory>

namespace lux::runtime::assets::pixel
{
    inline constexpr std::size_t kInfinite2DPixelPrepareQueueCapacity = 32u;
    inline constexpr std::size_t kInfinite2DPixelPrepareByteBudget =
        16u * 1024u * 1024u;
    inline constexpr std::size_t kInfinite2DPixelPrepareDrainBatch = 8u;

    class LUX_ENGINE_RUNTIME_PIXEL_ASSETS_PUBLIC
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

        [[nodiscard]] lux::ecs::pixel::streaming::
            Infinite2DPixelPrepareClient client() const noexcept;
        void close() noexcept;

    private:
        Infinite2DPixelPrepareService(
            std::shared_ptr<lux::ecs::pixel::streaming::detail::
                Infinite2DPixelPrepareState> state,
            lux::async::OperationPort<
                lux::ecs::pixel::streaming::PrepareInfinite2DPixelChunk>
                operation) noexcept;

        std::shared_ptr<lux::ecs::pixel::streaming::detail::
            Infinite2DPixelPrepareState> state_;
        lux::async::OperationPort<
            lux::ecs::pixel::streaming::PrepareInfinite2DPixelChunk>
            operation_;
        bool closed_{false};
    };
} // namespace lux::runtime::assets::pixel
