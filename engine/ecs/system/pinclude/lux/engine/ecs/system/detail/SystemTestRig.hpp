#pragma once

#include <lux/engine/ecs/SystemExecution.hpp>
#include <lux/engine/ecs/SystemRegistry.hpp>
#include <lux/engine/ecs/SystemRelations.hpp>
#include <lux/engine/ecs/SystemTaskGraphCompiler.hpp>
#include <lux/engine/ecs/system/detail/SystemExecutionTestAccess.hpp>

#include <cassert>
#include <memory>
#include <optional>
#include <utility>

namespace lux::ecs::detail
{
    class SystemTestRig final
    {
    public:
        explicit SystemTestRig(World& world)
            : world_(std::addressof(world)), relations_(systems_)
        {
        }

        template <System Type, class... Args>
        [[nodiscard]] SystemId add(Args&&... args)
        {
            auto result = systems_.emplace<Type>(
                std::forward<Args>(args)...
            );
            assert(result);
            return *result;
        }

        void before(SystemId before, SystemId after)
        {
            assert(relations_.before(before, after));
        }

        void after(SystemId after, SystemId before)
        {
            assert(relations_.after(after, before));
        }

        [[nodiscard]] bool compile(std::size_t reserve_records = 0U)
        {
            auto result = compiler_.compile(systems_, relations_);
            if (!result)
                return false;
            compilation_.emplace(std::move(*result));
            return static_cast<bool>(
                scratch_.prepare(*compilation_, reserve_records)
            );
        }

        [[nodiscard]] bool run(float delta_seconds, std::uint64_t tick)
        {
            assert(compilation_);
            EcsExecutionContext context(
                *world_,
                systems_,
                scratch_,
                delta_seconds,
                tick
            );
            return static_cast<bool>(executeSystemTaskGraph(
                lux::task::referenceTaskExecutionBackend(),
                *compilation_,
                context
            ));
        }

        template <class Type>
        [[nodiscard]] Type& system(SystemId id) noexcept
        {
            return SystemExecutionTestAccess::system<Type>(systems_, id);
        }

        void failNextCommandPush(SystemId id) noexcept
        {
            assert(compilation_);
            SystemExecutionTestAccess::failNextCommandPush(
                *compilation_,
                scratch_,
                id
            );
        }

        [[nodiscard]] bool erase(SystemId id) noexcept
        {
            compilation_.reset();
            return systems_.erase(id);
        }

        [[nodiscard]] std::size_t systemCapacity() const noexcept
        {
            return scratch_.systemCapacity();
        }

        [[nodiscard]] std::uint64_t laneBindCount() const noexcept
        {
            return scratch_.laneBindCount();
        }

        [[nodiscard]] std::uint64_t perRecordLookupCount() const noexcept
        {
            return scratch_.perRecordLookupCount();
        }

    private:
        World* world_{};
        SystemRegistry systems_;
        SystemRelations relations_;
        SystemTaskGraphCompiler compiler_;
        std::optional<SystemTaskGraphCompilation> compilation_;
        SystemExecutionScratch scratch_;
    };
}
