#pragma once

#include <lux/engine/render/core/Error.hpp>

namespace lux::render
{
    inline constexpr auto kTransportCapacity     = error::errorId("lux.render.transport.capacity");
    inline constexpr auto kTransportStopping     = error::errorId("lux.render.transport.stopping");
    inline constexpr auto kTransportWrongOwner   = error::errorId("lux.render.transport.wrong_owner");
    inline constexpr auto kTransportWrongThread  = error::errorId("lux.render.transport.wrong_thread");
    inline constexpr auto kTransportStaleRoute   = error::errorId("lux.render.transport.stale_route");
    inline constexpr auto kTransportUnknownRoute = error::errorId("lux.render.transport.unknown_route");
    inline constexpr auto kTransportContract     = error::errorId("lux.render.transport.contract");
    inline constexpr auto kTransportBounds       = error::errorId("lux.render.transport.bounds");
    inline constexpr auto kTransportReply        = error::errorId("lux.render.transport.reply");
    inline constexpr auto kTransportCancelled    = error::errorId("lux.render.transport.cancelled");
    inline constexpr auto kTransportBusy         = error::errorId("lux.render.transport.busy");
    inline constexpr auto kTransportByteBudget   = error::errorId("lux.render.transport.byte_budget");

    [[nodiscard]] std::span<const error::ErrorDescriptor> renderTransportErrorDescriptors() noexcept;
}
