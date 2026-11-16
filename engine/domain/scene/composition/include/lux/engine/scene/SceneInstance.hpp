#pragma once

#include <exec/task.hpp>
#include <lux/engine/core/execution/TaskScope.hpp>
#include <lux/engine/error/Error.hpp>
#include <lux/engine/ecs/EcsCommandBuffer.hpp>

#include <lux/engine/scene/visibility.h>

#include <memory>
#include <span>
#include <vector>

namespace lux::world
{
    class World;
}

namespace lux::simulation
{
    class Simulation;
}

namespace lux::engine
{
    class Engine;
}

namespace lux::ecs
{
    class ComponentSchemaSet;
}

namespace lux::scene
{
    class SceneSystem;

    // Owns an assembled scene. Systems are destroyed before the world they borrow.
    class LUX_ENGINE_SCENE_PUBLIC SceneInstance final
    {
    public:
        // Both arguments must be non-null and fully constructed.
        SceneInstance(
            std::unique_ptr<world::World> world,
            std::unique_ptr<simulation::Simulation> simulation,
            std::shared_ptr<const ecs::ComponentSchemaSet> schemas = {}
        ) noexcept;

        ~SceneInstance() noexcept;
        SceneInstance(const SceneInstance&) = delete;
        SceneInstance& operator=(const SceneInstance&) = delete;

        [[nodiscard]] world::World& world() noexcept;
        [[nodiscard]] simulation::Simulation& simulation() noexcept;

        [[nodiscard]] std::span<const std::unique_ptr<SceneSystem>> systems() const noexcept;

        // Takes a non-null system; its dependencies must outlive it.
        void add(std::unique_ptr<SceneSystem> system) noexcept;

    private:
        friend class engine::Engine;
        using Result = cxx::expected<void, error::Error>;
        
        void bindExecution(
            execution::TaskScope& tasks,
            ecs::EcsCommandBuffer& commands,
            std::span<const std::size_t> producers
        ) noexcept;
        
        [[nodiscard]] Result applyCommands(ecs::EcsCommandBuffer& commands) noexcept;
        [[nodiscard]] exec::task<Result> initialize(ecs::EcsCommandBuffer& commands) noexcept;
        [[nodiscard]] exec::task<Result> releaseResources() noexcept;

        // Systems may borrow the schemas supplied by the builder.
        std::shared_ptr<const ecs::ComponentSchemaSet> schemas_;
        std::unique_ptr<world::World> world_;
        std::unique_ptr<simulation::Simulation> simulation_;
        std::vector<std::unique_ptr<SceneSystem>> systems_;
    };
} // namespace lux::scene
