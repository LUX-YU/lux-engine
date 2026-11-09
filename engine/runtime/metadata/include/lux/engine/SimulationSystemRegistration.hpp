#pragma once

#include <lux/engine/error/Error.hpp>
#include <lux/engine/serialization/PortableValueCodec.hpp>
#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace lux::engine
{
    class EngineContext;
}
namespace lux::world
{
    class World;
}

namespace lux::simulation
{
    class SimulationSystem;

    // Cold construction entry. Registration never constructs an instance or starts borrowing work.
    struct SimulationSystemRegistration final
    {
        std::string name;
        std::uint32_t configuration_version{};
        serialization::PortableValueCodec configuration;
        cxx::expected<std::unique_ptr<SimulationSystem>, error::Error> (*create)(
            engine::EngineContext&, world::World&, std::span<const std::byte>
        ) noexcept {};
    };
} // namespace lux::simulation
