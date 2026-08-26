#pragma once

#include <lux/engine/simulation/ecs/EcsChangeJournal.hpp>
#include <lux/engine/simulation/ecs/EcsState.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace lux::simulation::ecs
{
    /**
     * Explicit Simulation adapter that couples canonical ECS mutation with one
     * lexical change-publication session. EcsMutation itself stays observation
     * free.
     */
    class LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC SimulationEcsMutation final
    {
      public:
        SimulationEcsMutation() noexcept;
        ~SimulationEcsMutation() noexcept = default;
        SimulationEcsMutation(SimulationEcsMutation&&) noexcept;
        SimulationEcsMutation& operator=(SimulationEcsMutation&&) noexcept;
        SimulationEcsMutation(const SimulationEcsMutation&) = delete;
        SimulationEcsMutation& operator=(const SimulationEcsMutation&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] const EcsState& state() const noexcept;

        [[nodiscard]] Entity create();
        void destroy(Entity entity);

        template <class Component, class... Args>
        Component& emplace(Entity entity, Args&&... args)
        {
            Component& result = mutation().template emplace<Component>(
                entity,
                std::forward<Args>(args)...
            );
            recordComponent(
                entt::type_hash<Component>::value(),
                entity,
                EComponentChangeKind::ADDED
            );
            return result;
        }

        template <class Component>
        void erase(Entity entity)
        {
            const bool existed = state().template find<Component>(entity) != nullptr;
            mutation().template erase<Component>(entity);
            if (existed)
            {
                recordComponent(
                    entt::type_hash<Component>::value(),
                    entity,
                    EComponentChangeKind::REMOVED
                );
            }
        }

        template <class Component, class Fn>
            requires std::is_nothrow_invocable_v<Fn, Component&>
        void update(Entity entity, Fn&& fn) noexcept
        {
            mutation().template update<Component>(
                entity,
                std::forward<Fn>(fn)
            );
            recordComponent(
                entt::type_hash<Component>::value(),
                entity,
                EComponentChangeKind::MODIFIED
            );
        }

        template <class... Access>
        [[nodiscard]] auto query()
        {
            static_assert((!detail::AccessTraits<Access>::kWrite && ...));
            return mutation().template query<Access...>();
        }

        template <class Component>
        void reserve(std::size_t count)
        {
            mutation().template reserve<Component>(count);
        }

      private:
        explicit SimulationEcsMutation(
            EcsState& state,
            EcsMutation&& mutation,
            EcsChangeJournal& journal
        ) noexcept;
        [[nodiscard]] EcsMutation& mutation() noexcept;
        void recordComponent(
            std::uint64_t storage,
            Entity entity,
            EComponentChangeKind kind
        ) noexcept;

        EcsState* state_{};
        EcsMutation mutation_;
        void* publisher_log_{};
        bool publisher_exact_{true};

        friend LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC
        lux::cxx::expected<SimulationEcsMutation, EcsMutationError>
        beginSimulationEcsMutation(
            EcsState&,
            EcsChangeJournal&
        ) noexcept;
    };

    [[nodiscard]] LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC
    lux::cxx::expected<SimulationEcsMutation, EcsMutationError>
    beginSimulationEcsMutation(
        EcsState& state,
        EcsChangeJournal& journal
    ) noexcept;
} // namespace lux::simulation::ecs
