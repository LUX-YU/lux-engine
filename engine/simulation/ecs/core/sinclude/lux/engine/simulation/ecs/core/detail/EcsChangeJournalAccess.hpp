#pragma once

#include <lux/engine/simulation/ecs/EcsChangeJournal.hpp>
#include <lux/engine/simulation/ecs/core/detail/EcsChangeLog.hpp>

namespace lux::simulation::ecs::detail
{
    struct EcsChangeJournalAccess final
    {
        [[nodiscard]] LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC
        static EcsChangeLog& log(
            EcsChangeJournal& journal
        ) noexcept;

        [[nodiscard]] LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC
        static const EcsChangeLog& log(
            const EcsChangeJournal& journal
        ) noexcept;
    };

    class EcsChangePublisher final
    {
      public:
        explicit EcsChangePublisher(EcsChangeJournal& journal) noexcept
            : log_(&EcsChangeJournalAccess::log(journal))
        {
        }

        [[nodiscard]] BoundEcsChangeStream bindComponent(
            std::uint64_t storage
        ) noexcept
        {
            if (!exact_)
                return {};
            BoundEcsChangeStream result = log_->bindComponent(storage);
            if (!result)
                exact_ = false;
            return result;
        }

        [[nodiscard]] bool append(
            BoundEcsChangeStream stream,
            Entity entity,
            EComponentChangeKind kind
        ) noexcept
        {
            if (!exact_)
                return false;
            exact_ = stream(entity, kind);
            return exact_;
        }

        [[nodiscard]] bool appendEntity(
            Entity entity,
            EEntityChangeKind kind
        ) noexcept
        {
            if (!exact_)
                return false;
            exact_ = log_->recordEntity(entity, kind);
            return exact_;
        }

        [[nodiscard]] bool exact() const noexcept
        {
            return exact_;
        }

      private:
        EcsChangeLog* log_{};
        bool exact_{true};
    };
} // namespace lux::simulation::ecs::detail
