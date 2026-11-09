#include <lux/engine/EngineMetaFunctionRegistry.hpp>

#include <lux/cxx/container/HeterogeneousLookup.hpp>
#include <lux/cxx/container/SparseSet.hpp>
#include <lux/engine/ecs/ComponentSchema.hpp>
#include <lux/engine/GeometryDeclaration.hpp>
#include <lux/engine/SceneSystemRegistration.hpp>
#include <lux/engine/SimulationSystemRegistration.hpp>
#include <lux/engine/PartitionDeclaration.hpp>

#include <cstddef>
#include <string>
#include <utility>

namespace lux::engine
{
    struct EngineMetaFunctionRegistry::Impl final
    {
        using NameIndex = cxx::heterogeneous_map<std::size_t>;

        cxx::AutoSparseSet<scene::GeometryDeclaration>               geometries;
        NameIndex                                                    geometry_names;

        cxx::AutoSparseSet<world::PartitionDeclaration>              partitions;
        NameIndex                                                    partition_names;

        cxx::AutoSparseSet<ecs::ComponentSchema>                     components;
        NameIndex                                                    component_names;

        cxx::AutoSparseSet<simulation::SimulationSystemRegistration> simulation_systems;
        NameIndex                                                    simulation_system_names;

        cxx::AutoSparseSet<scene::SceneSystemRegistration>           scene_systems;
        NameIndex                                                    scene_system_names;

        template <class T>
        [[nodiscard]] static bool
        add(cxx::AutoSparseSet<T>& records, NameIndex& names, std::string_view name, T&& declaration) noexcept
        {
            const bool is_unavailable_name = name.empty() || names.contains(name);
            if (is_unavailable_name)
            {
                return false;
            }
            // Copy before sparse-set growth or moving declaration can invalidate the name borrow.
            std::string owned_name{name};
            const auto key = records.emplace(std::move(declaration));
            names.emplace(std::move(owned_name), key);
            return true;
        }

        template <class T>
        [[nodiscard]] static const T*
        find(const cxx::AutoSparseSet<T>& records, const NameIndex& names, std::string_view name) noexcept
        {
            const auto found = names.find(name);
            return found == names.end() ? nullptr : records.tryGet(found->second);
        }

        template <class T>
        [[nodiscard]] static bool
        remove(cxx::AutoSparseSet<T>& records, NameIndex& names, std::string_view name) noexcept
        {
            const auto found = names.find(name);
            if (found == names.end())
            {
                return false;
            }
            const auto key = found->second;
            names.erase(found);
            records.erase(key);
            return true;
        }
    };

    EngineMetaFunctionRegistry::EngineMetaFunctionRegistry() : impl_(std::make_unique<Impl>()) {}

    EngineMetaFunctionRegistry::~EngineMetaFunctionRegistry() = default;

    bool EngineMetaFunctionRegistry::addGeometry(scene::GeometryDeclaration declaration) noexcept
    {
        return Impl::add(impl_->geometries, impl_->geometry_names, declaration.name, std::move(declaration));
    }

    const scene::GeometryDeclaration* EngineMetaFunctionRegistry::findGeometry(std::string_view name) const noexcept
    {
        return Impl::find(impl_->geometries, impl_->geometry_names, name);
    }

    std::span<const scene::GeometryDeclaration> EngineMetaFunctionRegistry::geometries() const noexcept
    {
        return impl_->geometries.values();
    }

    bool EngineMetaFunctionRegistry::removeGeometry(std::string_view name) noexcept
    {
        return Impl::remove(impl_->geometries, impl_->geometry_names, name);
    }

    bool EngineMetaFunctionRegistry::addPartitioner(world::PartitionDeclaration declaration) noexcept
    {
        return Impl::add(impl_->partitions, impl_->partition_names, declaration.name, std::move(declaration));
    }

    const world::PartitionDeclaration* EngineMetaFunctionRegistry::findPartitioner(std::string_view name) const noexcept
    {
        return Impl::find(impl_->partitions, impl_->partition_names, name);
    }

    std::span<const world::PartitionDeclaration> EngineMetaFunctionRegistry::partitioners() const noexcept
    {
        return impl_->partitions.values();
    }

    bool EngineMetaFunctionRegistry::removePartitioner(std::string_view name) noexcept
    {
        return Impl::remove(impl_->partitions, impl_->partition_names, name);
    }

    bool EngineMetaFunctionRegistry::addComponent(ecs::ComponentSchema declaration) noexcept
    {
        return Impl::add(impl_->components, impl_->component_names, declaration.id.name, std::move(declaration));
    }

    const ecs::ComponentSchema* EngineMetaFunctionRegistry::findComponent(std::string_view name) const noexcept
    {
        return Impl::find(impl_->components, impl_->component_names, name);
    }

    std::span<const ecs::ComponentSchema> EngineMetaFunctionRegistry::components() const noexcept
    {
        return impl_->components.values();
    }

    bool EngineMetaFunctionRegistry::removeComponent(std::string_view name) noexcept
    {
        return Impl::remove(impl_->components, impl_->component_names, name);
    }

    bool EngineMetaFunctionRegistry::addSimulationSystem(simulation::SimulationSystemRegistration declaration) noexcept
    {
        return Impl::add(
            impl_->simulation_systems,
            impl_->simulation_system_names,
            declaration.name,
            std::move(declaration)
        );
    }

    const simulation::SimulationSystemRegistration*
    EngineMetaFunctionRegistry::findSimulationSystem(std::string_view name) const noexcept
    {
        return Impl::find(impl_->simulation_systems, impl_->simulation_system_names, name);
    }

    std::span<const simulation::SimulationSystemRegistration>
    EngineMetaFunctionRegistry::simulationSystems() const noexcept
    {
        return impl_->simulation_systems.values();
    }

    bool EngineMetaFunctionRegistry::removeSimulationSystem(std::string_view name) noexcept
    {
        return Impl::remove(impl_->simulation_systems, impl_->simulation_system_names, name);
    }

    bool EngineMetaFunctionRegistry::addSceneSystem(scene::SceneSystemRegistration declaration) noexcept
    {
        return Impl::add(
            impl_->scene_systems,
            impl_->scene_system_names,
            declaration.name,
            std::move(declaration)
        );
    }

    const scene::SceneSystemRegistration*
    EngineMetaFunctionRegistry::findSceneSystem(std::string_view name) const noexcept
    {
        return Impl::find(impl_->scene_systems, impl_->scene_system_names, name);
    }

    std::span<const scene::SceneSystemRegistration> EngineMetaFunctionRegistry::sceneSystems() const noexcept
    {
        return impl_->scene_systems.values();
    }

    bool EngineMetaFunctionRegistry::removeSceneSystem(std::string_view name) noexcept
    {
        return Impl::remove(impl_->scene_systems, impl_->scene_system_names, name);
    }
} // namespace lux::engine
