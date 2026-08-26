#pragma once

#include <lux/engine/simulation/ecs/HierarchyIndex.hpp>
#include <lux/engine/simulation/ecs/EcsTaskAccess.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/EcsTaskResources.hpp>
#include <lux/engine/simulation/ecs/transform/visibility.h>

#include <cstddef>
#include <memory>

namespace lux::simulation::ecs
{
    namespace detail
    {
        struct TransformSystemTestAccess;
    }

    class LUX_ENGINE_SIMULATION_ECS_TRANSFORM_PUBLIC Transform2DSystem final
    {
      public:
        inline static constexpr auto Access = makeSystemAccessSpec<
            Read<Transform2D>,
            Write<WorldTransform2D>,
            ExternalRead<HierarchyIndex>,
            ExternalRead<HierarchyDeltaBatch>>();
        inline static constexpr auto TaskAccess = access<
            Read<Transform2D>,
            Write<WorldTransform2D>,
            ExternalRead<HierarchyIndex>,
            ExternalRead<HierarchyDeltaBatch>>;
        inline static constexpr auto EcsChangesAccess = ecsChangesRead();

        Transform2DSystem(
            HierarchyIndex& hierarchy,
            const HierarchyDeltaBatch& hierarchy_deltas
        );
        ~Transform2DSystem();

        void update(
            const EcsState& state,
            EcsChangeJournal& journal,
            TaskWriter<WorldTransform2D>& writer,
            EcsCommands commands
        ) noexcept;
        void invokeTask(
            EcsState& state,
            EcsChangeJournal& journal,
            EcsChangeBatch& changes,
            EcsCommands commands
        ) noexcept
        {
            auto writer = taskWriter<WorldTransform2D>(
                state,
                changes,
                TaskAccess
            );
            update(state, journal, writer, commands);
        }

      private:
        [[nodiscard]] std::size_t visitedNodesLastUpdate() const noexcept;
        [[nodiscard]] std::size_t retainedDenseBytes() const noexcept;

        struct Impl;
        std::unique_ptr<Impl> impl_;

        friend struct detail::TransformSystemTestAccess;
    };

    class LUX_ENGINE_SIMULATION_ECS_TRANSFORM_PUBLIC Transform3DSystem final
    {
      public:
        inline static constexpr auto Access = makeSystemAccessSpec<
            Read<Transform3D>,
            Write<WorldTransform3D>,
            ExternalRead<HierarchyIndex>,
            ExternalRead<HierarchyDeltaBatch>>();
        inline static constexpr auto TaskAccess = access<
            Read<Transform3D>,
            Write<WorldTransform3D>,
            ExternalRead<HierarchyIndex>,
            ExternalRead<HierarchyDeltaBatch>>;
        inline static constexpr auto EcsChangesAccess = ecsChangesRead();

        Transform3DSystem(
            HierarchyIndex& hierarchy,
            const HierarchyDeltaBatch& hierarchy_deltas
        );
        ~Transform3DSystem();

        void update(
            const EcsState& state,
            EcsChangeJournal& journal,
            TaskWriter<WorldTransform3D>& writer,
            EcsCommands commands
        ) noexcept;
        void invokeTask(
            EcsState& state,
            EcsChangeJournal& journal,
            EcsChangeBatch& changes,
            EcsCommands commands
        ) noexcept
        {
            auto writer = taskWriter<WorldTransform3D>(
                state,
                changes,
                TaskAccess
            );
            update(state, journal, writer, commands);
        }

      private:
        [[nodiscard]] std::size_t visitedNodesLastUpdate() const noexcept;
        [[nodiscard]] std::size_t retainedDenseBytes() const noexcept;

        struct Impl;
        std::unique_ptr<Impl> impl_;

        friend struct detail::TransformSystemTestAccess;
    };
} // namespace lux::simulation::ecs
