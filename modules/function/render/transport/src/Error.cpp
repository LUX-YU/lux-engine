#include <lux/engine/render/transport/Error.hpp>

namespace lux::render
{
    std::span<const error::ErrorDescriptor> renderTransportErrorDescriptors() noexcept
    {
        static constexpr std::array descriptors{
            error::ErrorDescriptor{"lux.render.transport.capacity", "Bounded capacity exhausted"},
            error::ErrorDescriptor{"lux.render.transport.stopping", "Transport is stopping"},
            error::ErrorDescriptor{"lux.render.transport.wrong_owner", "Different transport owner"},
            error::ErrorDescriptor{"lux.render.transport.wrong_thread", "Wrong endpoint thread"},
            error::ErrorDescriptor{"lux.render.transport.stale_route", "Stale route generation"},
            error::ErrorDescriptor{"lux.render.transport.unknown_route", "Unknown route"},
            error::ErrorDescriptor{"lux.render.transport.contract", "Operation contract mismatch"},
            error::ErrorDescriptor{"lux.render.transport.bounds", "Payload range or layout mismatch"},
            error::ErrorDescriptor{"lux.render.transport.reply", "Reply contract or generation mismatch"},
            error::ErrorDescriptor{"lux.render.transport.cancelled", "Packet or deferred completion cancelled"},
            error::ErrorDescriptor{"lux.render.transport.busy", "Contended upload admission or reentrant consumer"},
            error::ErrorDescriptor{"lux.render.transport.byte_budget", "Upload byte budget exhausted"}
        };
        return descriptors;
    }
}
