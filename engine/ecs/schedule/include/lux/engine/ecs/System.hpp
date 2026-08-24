#pragma once

#include <lux/engine/ecs/SystemAccess.hpp>
#include <lux/engine/ecs/SystemFrame.hpp>
#include <lux/engine/ecs/SystemOrder.hpp>
#include <lux/engine/ecs/schedule/visibility.h>

#include <lux/cxx/compile_time/TypeToken.hpp>

#include <entt/signal/sigh.hpp>

#include <functional>
#include <span>
#include <type_traits>
#include <vector>

namespace lux::ecs
{
    class SystemAttach final
    {
      public:
        [[nodiscard]] World& world() const noexcept
        {
            return *world_;
        }

        [[nodiscard]] WorldCommands commands() const noexcept
        {
            return commands_;
        }

        template <class Component, auto Candidate, class Instance>
        void observeConstruct(Instance& instance)
        {
            static_assert(std::is_invocable_v<decltype(Candidate), Instance&, Entity>);
            static_assert(std::is_nothrow_invocable_v<decltype(Candidate), Instance&, Entity>);
            observers_->emplace_back(
                world_->template connectConstruct<Component, Candidate>(instance)
            );
            ++world_->observer_relations_;
            for (auto [entity, component] :
                 world_->template query<Read<Component>>())
            {
                (void)component;
                std::invoke(Candidate, instance, entity);
            }
        }

        template <class Component, auto Candidate, class Instance>
        void observeUpdate(Instance& instance)
        {
            static_assert(std::is_invocable_v<decltype(Candidate), Instance&, Entity>);
            static_assert(std::is_nothrow_invocable_v<decltype(Candidate), Instance&, Entity>);
            observers_->emplace_back(
                world_->template connectUpdate<Component, Candidate>(instance)
            );
            ++world_->observer_relations_;
        }

        template <class Component, auto Candidate, class Instance>
        void observeDestroy(Instance& instance)
        {
            static_assert(std::is_invocable_v<decltype(Candidate), Instance&, Entity>);
            static_assert(std::is_nothrow_invocable_v<decltype(Candidate), Instance&, Entity>);
            observers_->emplace_back(
                world_->template connectDestroy<Component, Candidate>(instance)
            );
            ++world_->observer_relations_;
        }

      private:
        SystemAttach(
            World& world,
            WorldCommands commands,
            std::vector<entt::scoped_connection>& observers
        ) noexcept
            : world_(&world), commands_(commands), observers_(&observers)
        {
        }

        World* world_{};
        WorldCommands commands_{};
        std::vector<entt::scoped_connection>* observers_{};

        friend class Schedule;
        friend class ScheduleEdit;
    };

    class SystemDetach final
    {
      public:
        [[nodiscard]] World& world() const noexcept
        {
            return *world_;
        }

      private:
        explicit SystemDetach(World& world) noexcept : world_(&world) {}

        World* world_{};

        friend class Schedule;
        friend class ScheduleEdit;
    };

    class LUX_ENGINE_ECS_SCHEDULE_PUBLIC System
    {
      public:
        virtual ~System() = default;

        virtual void onAttach(SystemAttach&) noexcept {}
        virtual void onDetach(SystemDetach&) noexcept {}
        virtual void update(const SystemFrame&) noexcept = 0;

        virtual void requestClose() noexcept {}

        [[nodiscard]] virtual bool closeComplete() const noexcept
        {
            return true;
        }

        [[nodiscard]] virtual bool removable() const noexcept
        {
            return false;
        }

        [[nodiscard]] virtual SystemAccess access() const noexcept
        {
            return {};
        }

        [[nodiscard]] virtual std::span<const SystemSetId>
        sets() const noexcept
        {
            return {};
        }

        [[nodiscard]] virtual std::span<const SystemOrder>
        ordering() const noexcept
        {
            return {};
        }

        [[nodiscard]] virtual std::span<const lux::cxx::TypeToken>
        requiredSystems() const noexcept
        {
            return {};
        }
    };
} // namespace lux::ecs
