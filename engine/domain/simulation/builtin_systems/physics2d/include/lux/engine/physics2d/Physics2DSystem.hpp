#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/physics2d/Physics2DComponents.hpp>
#include <lux/engine/physics2d/abilities/PhysicsQuery2D.hpp>
#include <lux/engine/physics2d/visibility.h>
#include <lux/engine/simulation/SimulationSystemDescription.hpp>
#include <lux/engine/simulation/SimulationSystemRegistry.hpp>
#include <lux/engine/simulation/SimulationTime.hpp>
#include <lux/engine/simulation/SystemAccessSpec.hpp>
#include <lux/engine/simulation/ecs/Registry.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace lux::physics2d
{
    enum class EPhysics2DSystemError : std::uint8_t
    {
        INVALID_CONFIGURATION,
        CAPACITY_EXCEEDED,
        INVALID_COMPONENT,
    };

    struct LUX_TYPE_INFO(both) Physics2DSystemConfiguration final
    {
        double gravity_x{};
        double gravity_y{-9.81};

        LUX_MEMBER(min = 1)
        std::int64_t fixed_step_nanoseconds{16'666'667};

        LUX_MEMBER(min = 1)
        std::uint32_t max_substeps{8U};

        LUX_MEMBER(min = 1)
        std::uint64_t body_capacity{16'384U};
    };

    struct Physics2DRuntimeStats final
    {
        std::size_t active_bodies{};
        std::uint64_t completed_steps{};
        std::uint64_t overlap_queries{};
    };

    class LUX_ENGINE_PHYSICS2D_SIMULATION_PUBLIC Physics2DSystem final
    {
    public:
        inline static constexpr std::array Capabilities{
            std::string_view{"physics.2d"},
            std::string_view{"lux.physics2d.query"}
        };
        inline static constexpr auto Access = lux::simulation::makeSystemAccessSpec<
            lux::simulation::TComponentRead<BoxCollider2D>,
            lux::simulation::TComponentWrite<RigidBody2D>,
            lux::simulation::TComponentWrite<lux::simulation::ecs::Transform2D>>();
        inline static constexpr std::string_view SupportedWorldTypes[]{"*"};
        inline static constexpr lux::simulation::SimulationSystemDescription Description{
            .type =
                {.canonical_name = "lux.physics2d.System",
                 .version = 1U,
                 .configuration_schema_name = "lux.physics2d.Configuration",
                 .configuration_schema_version = 1U,
                 .capabilities = Capabilities,
                 .supported_world_types = SupportedWorldTypes}
        };

        using CreateResult = lux::cxx::expected<std::unique_ptr<Physics2DSystem>, EPhysics2DSystemError>;
        [[nodiscard]] static CreateResult create(
            lux::simulation::ecs::Registry& registry,
            const lux::simulation::SimulationTime& time,
            Physics2DSystemConfiguration configuration
        ) noexcept;
        ~Physics2DSystem() noexcept;

        Physics2DSystem(const Physics2DSystem&) = delete;
        Physics2DSystem& operator=(const Physics2DSystem&) = delete;
        Physics2DSystem(Physics2DSystem&&) = delete;
        Physics2DSystem& operator=(Physics2DSystem&&) = delete;

        [[nodiscard]] bool update() noexcept;

        [[nodiscard]] bool overlapsBox(
            double center_x,
            double center_y,
            double half_width,
            double half_height
        ) noexcept;

        [[nodiscard]] Physics2DRuntimeStats stats() const noexcept;

    private:
        struct Impl;
        explicit Physics2DSystem(std::unique_ptr<Impl>) noexcept;
        std::unique_ptr<Impl> impl_;
    };

    [[nodiscard]] LUX_ENGINE_PHYSICS2D_SIMULATION_PUBLIC const lux::simulation::SimulationSystemDescription&
    physics2DSystemDescription() noexcept;

    [[nodiscard]] LUX_ENGINE_PHYSICS2D_SIMULATION_PUBLIC std::span<const lux::simulation::SimulationSystemRegistration>
    physics2DSystemRegistrations() noexcept;

    [[nodiscard]] LUX_ENGINE_PHYSICS2D_SIMULATION_PUBLIC lux::cxx::
        expected<std::vector<std::byte>, EPhysics2DSystemError>
        makePhysics2DSystemConfiguration(const Physics2DSystemConfiguration& configuration) noexcept;
} // namespace lux::physics2d
