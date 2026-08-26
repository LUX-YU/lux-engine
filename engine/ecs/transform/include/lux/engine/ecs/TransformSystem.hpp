#pragma once

#include <lux/engine/ecs/HierarchyIndex.hpp>
#include <lux/engine/ecs/EcsTaskAccess.hpp>
#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/ecs/EcsTaskResources.hpp>
#include <lux/engine/ecs/transform/visibility.h>

#include <cstddef>
#include <memory>

namespace lux::ecs
{
    namespace detail
    {
        struct TransformSystemTestAccess;
    }

    class LUX_ENGINE_ECS_TRANSFORM_PUBLIC Transform2DSystem final
    {
      public:
        inline static constexpr auto Access = makeSystemAccessSpec<
            Read<Transform2D>,
            Write<WorldTransform2D>,
            ExternalRead<HierarchyIndex>>();
        inline static constexpr auto TaskAccess = access<
            Read<Transform2D>,
            Write<WorldTransform2D>,
            ExternalRead<HierarchyIndex>>;

        explicit Transform2DSystem(HierarchyIndex& hierarchy);
        ~Transform2DSystem();

        void update(
            const EcsState& world,
            TaskWriter<WorldTransform2D>& writer,
            EcsCommands commands
        ) noexcept;
        void invokeTask(
            EcsState& world,
            EcsChangeBatch& changes,
            EcsCommands commands
        ) noexcept
        {
            auto writer = taskWriter<WorldTransform2D>(
                world,
                changes,
                TaskAccess
            );
            update(world, writer, commands);
        }

      private:
        [[nodiscard]] std::size_t visitedNodesLastUpdate() const noexcept;
        [[nodiscard]] std::size_t retainedDenseBytes() const noexcept;

        struct Impl;
        std::unique_ptr<Impl> impl_;

        friend struct detail::TransformSystemTestAccess;
    };

    class LUX_ENGINE_ECS_TRANSFORM_PUBLIC Transform3DSystem final
    {
      public:
        inline static constexpr auto Access = makeSystemAccessSpec<
            Read<Transform3D>,
            Write<WorldTransform3D>,
            ExternalRead<HierarchyIndex>>();
        inline static constexpr auto TaskAccess = access<
            Read<Transform3D>,
            Write<WorldTransform3D>,
            ExternalRead<HierarchyIndex>>;

        explicit Transform3DSystem(HierarchyIndex& hierarchy);
        ~Transform3DSystem();

        void update(
            const EcsState& world,
            TaskWriter<WorldTransform3D>& writer,
            EcsCommands commands
        ) noexcept;
        void invokeTask(
            EcsState& world,
            EcsChangeBatch& changes,
            EcsCommands commands
        ) noexcept
        {
            auto writer = taskWriter<WorldTransform3D>(
                world,
                changes,
                TaskAccess
            );
            update(world, writer, commands);
        }

      private:
        [[nodiscard]] std::size_t visitedNodesLastUpdate() const noexcept;
        [[nodiscard]] std::size_t retainedDenseBytes() const noexcept;

        struct Impl;
        std::unique_ptr<Impl> impl_;

        friend struct detail::TransformSystemTestAccess;
    };
} // namespace lux::ecs
