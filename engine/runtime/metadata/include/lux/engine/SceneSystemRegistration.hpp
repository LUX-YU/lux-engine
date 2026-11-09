#pragma once

#include <lux/engine/error/Error.hpp>
#include <lux/engine/serialization/PortableValueCodec.hpp>
#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace lux::ecs
{
    class ComponentSchemaSet;
}

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
    class Simulation;
}

namespace lux::scene
{
    class SceneSystem;
    // Construction borrows only common dependencies. Asset identities belong to each System's configuration.
    struct SceneSystemRegistration final
    {
        std::string name;
        std::uint32_t configuration_version{};
        serialization::PortableValueCodec configuration;
        cxx::expected<std::unique_ptr<SceneSystem>, error::Error> (*create)(
            engine::EngineContext&, world::World&, simulation::Simulation&, const ecs::ComponentSchemaSet&,
            std::span<const std::byte>
        ) noexcept {};
    };
} // namespace lux::scene
