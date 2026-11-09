#pragma once

#include <memory>
#include <span>
#include <string_view>

namespace lux
{
    namespace ecs
    {
        struct ComponentSchema;
    }

    namespace scene
    {
        struct GeometryDeclaration;
        struct SceneSystemRegistration;
    }

    namespace simulation
    {
        struct SimulationSystemRegistration;
    }

    namespace world
    {
        struct PartitionDeclaration;
    }
}

namespace lux::engine
{
    // Cold directory of declarations and their operation entries, never of running objects.
    // Register/remove only before an Engine uses this directory. The directory, borrowed declaration
    // data and callback code must outlive every consumer. No registration invokes user callbacks.
    class EngineMetaFunctionRegistry final
    {
    public:
        EngineMetaFunctionRegistry();
        ~EngineMetaFunctionRegistry();
        EngineMetaFunctionRegistry(const EngineMetaFunctionRegistry&) = delete;
        EngineMetaFunctionRegistry& operator=(const EngineMetaFunctionRegistry&) = delete;

        // add*: owns the record; spans/pointers inside existing declarations remain borrowed.
        // Returns false for an empty or already registered name; the directory remains unchanged.
        // Declaration qualification belongs to its domain, not to this name directory.
        // find*: returns nullptr for an unknown name. Enumeration order is not semantic.
        // All returned pointers/spans are invalidated by a mutation of the corresponding category.
        // remove*: returns false for an unknown name; never destroys a running System or Scene.

        [[nodiscard]] bool addGeometry(scene::GeometryDeclaration declaration) noexcept;
        [[nodiscard]] const scene::GeometryDeclaration* findGeometry(std::string_view name) const noexcept;
        [[nodiscard]] std::span<const scene::GeometryDeclaration> geometries() const noexcept;
        [[nodiscard]] bool removeGeometry(std::string_view name) noexcept;

        [[nodiscard]] bool addPartitioner(world::PartitionDeclaration declaration) noexcept;
        [[nodiscard]] const world::PartitionDeclaration* findPartitioner(std::string_view name) const noexcept;
        [[nodiscard]] std::span<const world::PartitionDeclaration> partitioners() const noexcept;
        [[nodiscard]] bool removePartitioner(std::string_view name) noexcept;

        [[nodiscard]] bool addComponent(ecs::ComponentSchema declaration) noexcept;
        [[nodiscard]] const ecs::ComponentSchema* findComponent(std::string_view name) const noexcept;
        [[nodiscard]] std::span<const ecs::ComponentSchema> components() const noexcept;
        [[nodiscard]] bool removeComponent(std::string_view name) noexcept;

        [[nodiscard]] bool addSimulationSystem(simulation::SimulationSystemRegistration declaration) noexcept;
        [[nodiscard]] const simulation::SimulationSystemRegistration*
        findSimulationSystem(std::string_view name) const noexcept;
        [[nodiscard]] std::span<const simulation::SimulationSystemRegistration> simulationSystems() const noexcept;
        [[nodiscard]] bool removeSimulationSystem(std::string_view name) noexcept;

        [[nodiscard]] bool addSceneSystem(scene::SceneSystemRegistration declaration) noexcept;
        [[nodiscard]] const scene::SceneSystemRegistration* findSceneSystem(std::string_view name) const noexcept;
        [[nodiscard]] std::span<const scene::SceneSystemRegistration> sceneSystems() const noexcept;
        [[nodiscard]] bool removeSceneSystem(std::string_view name) noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::engine
