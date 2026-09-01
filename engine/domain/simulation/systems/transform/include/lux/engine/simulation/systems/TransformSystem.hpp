#pragma once

#include <lux/engine/simulation/SystemAccessSpec.hpp>
#include <lux/engine/simulation/SimulationSystemDescription.hpp>
#include <lux/engine/simulation/SimulationSystemRegistry.hpp>
#include <lux/engine/simulation/ecs/EcsCommandBuffer.hpp>
#include <lux/engine/simulation/ecs/HierarchyIndex.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/systems/transform/visibility.h>
#include <lux/engine/meta/MetaAnnotations.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace lux::simulation
{
    namespace detail
    {
        struct TransformSystemTestAccess;
    }

    enum class ETransformUpdateError : std::uint8_t
    {
        INVALID_HIERARCHY,
        CAPACITY_EXCEEDED,
        COMMAND_RECORDING_FAILED,
        ALLOCATION_FAILURE,
    };

    class LUX_ENGINE_SIMULATION_TRANSFORM_SYSTEM_PUBLIC Transform2DSystem final
    {
    public:
        inline static constexpr std::array Capabilities{std::string_view{"transform.2d"}};
        inline static constexpr auto Access = makeSystemAccessSpec<
            ComponentRead<ecs::Transform2D>,
            ComponentWrite<ecs::WorldTransform2D>,
            ExternalRead<ecs::HierarchyIndex>,
            ExternalRead<ecs::HierarchyDeltaBatch>>();
        inline static constexpr lux::simulation::SimulationSystemDescription Description{
            .type = {
                .canonical_name = "lux.transform2d",
                .version = 1,
                .capabilities = Capabilities
            }
        };

        Transform2DSystem(
            ecs::Registry& registry,
            ecs::HierarchyIndex& hierarchy,
            const ecs::HierarchyDeltaBatch& hierarchy_deltas
        );
        ~Transform2DSystem() noexcept;

        [[nodiscard]] lux::cxx::expected<void, ETransformUpdateError> prepare(std::size_t entity_capacity) noexcept;
        [[nodiscard]] lux::cxx::expected<void, ETransformUpdateError> update(
            ecs::EcsCommandWriter& commands
        ) noexcept;

    private:
        [[nodiscard]] std::size_t visitedNodesLastUpdate() const noexcept;
        [[nodiscard]] std::size_t retainedDenseBytes() const noexcept;
        struct Impl;
        std::unique_ptr<Impl> impl_;
        friend struct detail::TransformSystemTestAccess;
    };

    class LUX_ENGINE_SIMULATION_TRANSFORM_SYSTEM_PUBLIC Transform3DSystem final
    {
    public:
        inline static constexpr std::array Capabilities{std::string_view{"transform.3d"}};
        inline static constexpr auto Access = makeSystemAccessSpec<
            ComponentRead<ecs::Transform3D>,
            ComponentWrite<ecs::WorldTransform3D>,
            ExternalRead<ecs::HierarchyIndex>,
            ExternalRead<ecs::HierarchyDeltaBatch>>();
        inline static constexpr lux::simulation::SimulationSystemDescription Description{
            .type = {
                .canonical_name = "lux.transform3d",
                .version = 1,
                .capabilities = Capabilities
            }
        };

        Transform3DSystem(
            ecs::Registry& registry,
            ecs::HierarchyIndex& hierarchy,
            const ecs::HierarchyDeltaBatch& hierarchy_deltas
        );
        ~Transform3DSystem() noexcept;

        [[nodiscard]] lux::cxx::expected<void, ETransformUpdateError> prepare(std::size_t entity_capacity) noexcept;
        [[nodiscard]] lux::cxx::expected<void, ETransformUpdateError> update(
            ecs::EcsCommandWriter& commands
        ) noexcept;

    private:
        [[nodiscard]] std::size_t visitedNodesLastUpdate() const noexcept;
        [[nodiscard]] std::size_t retainedDenseBytes() const noexcept;
        struct Impl;
        std::unique_ptr<Impl> impl_;
        friend struct detail::TransformSystemTestAccess;
    };

    struct LUX_TYPE_INFO(both) TransformSystemConfiguration final
    {
        LUX_MEMBER(min = 1)
        std::uint64_t entity_capacity{};

        LUX_MEMBER(min = 1)
        std::uint64_t max_commands{};

        LUX_MEMBER(min = 1)
        std::uint64_t max_payload_bytes{};
    };

    [[nodiscard]] LUX_ENGINE_SIMULATION_TRANSFORM_SYSTEM_PUBLIC const SimulationSystemDescription&
    transformSystemDescription() noexcept;

    [[nodiscard]] LUX_ENGINE_SIMULATION_TRANSFORM_SYSTEM_PUBLIC std::span<const SimulationSystemRegistration>
    transformSystemRegistrations() noexcept;

    [[nodiscard]] LUX_ENGINE_SIMULATION_TRANSFORM_SYSTEM_PUBLIC lux::cxx::expected<
        std::vector<std::byte>,
        ETransformUpdateError
    > makeTransformSystemConfiguration(
        std::size_t entity_capacity,
        ecs::EcsCommandProducerCapacity command_capacity
    ) noexcept;
}
