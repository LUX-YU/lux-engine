#include <lux/engine/scene/SceneDescriptionBuilder.hpp>

#include <lux/engine/system/detail/SystemDependencyOrder.hpp>

#include <algorithm>
#include <limits>
#include <new>
#include <tuple>
#include <utility>
#include <vector>

namespace lux::scene
{
    struct SceneDescriptionBuilder::Impl final
    {
        struct PendingSystem final
        {
            system::SystemInstanceId id{};
            std::string instance_name;
            system::SystemTypeId type;
            std::uint32_t version{};
            std::string configuration_schema_name;
            std::uint32_t configuration_schema_version{};
            std::vector<std::byte> configuration;
        };
        struct PendingBinding final
        {
            system::SystemInstanceId system{};
            std::string requirement;
            std::string provider;
        };
        struct PendingDependency final
        {
            system::SystemInstanceId before{};
            system::SystemInstanceId after{};
        };

        asset::AssetId world{};
        asset::AssetId simulation{};
        std::vector<PendingSystem> systems;
        std::vector<PendingBinding> bindings;
        std::vector<PendingDependency> dependencies;
    };

    namespace
    {
        [[nodiscard]] SceneDescriptionFailure failure(
            ESceneDescriptionError code,
            system::SystemInstanceId system = {},
            std::uint64_t subject_hash = 0U
        ) noexcept
        {
            return {code, system, subject_hash};
        }

        template <class Range>
        [[nodiscard]] auto findSystem(Range& systems, system::SystemInstanceId id) noexcept
        {
            return std::find_if(systems.begin(), systems.end(), [id](const auto& value) noexcept {
                return value.id == id;
            });
        }
    } // namespace

    SceneDescriptionBuilder::SceneDescriptionBuilder() : impl_(std::make_unique<Impl>())
    {
    }

    SceneDescriptionBuilder::~SceneDescriptionBuilder() = default;
    SceneDescriptionBuilder::SceneDescriptionBuilder(SceneDescriptionBuilder&&) noexcept = default;
    SceneDescriptionBuilder& SceneDescriptionBuilder::operator=(SceneDescriptionBuilder&&) noexcept = default;

    void SceneDescriptionBuilder::setWorld(asset::AssetId id) noexcept
    {
        impl_->world = id;
    }

    void SceneDescriptionBuilder::setSimulation(asset::AssetId id) noexcept
    {
        impl_->simulation = id;
    }

    lux::cxx::expected<void, SceneDescriptionFailure> SceneDescriptionBuilder::addSystem(
        system::SystemInstanceId id,
        std::string_view instance_name,
        const system::SystemTypeId& type,
        std::uint32_t system_version,
        std::string_view configuration_schema_name,
        std::uint32_t configuration_schema_version,
        std::span<const std::byte> configuration
    ) noexcept
    {
        if (!id.valid())
        {
            return lux::cxx::unexpected(failure(ESceneDescriptionError::INVALID_SYSTEM_INSTANCE, id));
        }
        if (instance_name.empty())
        {
            return lux::cxx::unexpected(failure(ESceneDescriptionError::INVALID_SYSTEM_INSTANCE_NAME, id));
        }
        if (!type.valid())
        {
            return lux::cxx::unexpected(failure(ESceneDescriptionError::INVALID_SYSTEM_TYPE, id));
        }
        if (system_version == 0U)
        {
            return lux::cxx::unexpected(failure(ESceneDescriptionError::INVALID_SYSTEM_VERSION, id));
        }
        const bool has_schema = !configuration_schema_name.empty();
        if (has_schema != (configuration_schema_version != 0U) || (!has_schema && !configuration.empty()))
        {
            return lux::cxx::unexpected(failure(ESceneDescriptionError::INVALID_CONFIGURATION_SCHEMA, id));
        }
        if (findSystem(impl_->systems, id) != impl_->systems.end())
        {
            return lux::cxx::unexpected(failure(ESceneDescriptionError::DUPLICATE_SYSTEM_INSTANCE, id));
        }
        const bool duplicate_name = std::ranges::any_of(impl_->systems, [instance_name](const auto& system) noexcept {
            return system.instance_name == instance_name;
        });
        if (duplicate_name)
        {
            return lux::cxx::unexpected(failure(ESceneDescriptionError::DUPLICATE_SYSTEM_INSTANCE_NAME, id));
        }
        try
        {
            impl_->systems.push_back(Impl::PendingSystem{
                id,
                std::string(instance_name),
                type,
                system_version,
                std::string(configuration_schema_name),
                configuration_schema_version,
                std::vector<std::byte>(configuration.begin(), configuration.end())
            });
            return {};
        }
        catch (const std::length_error&)
        {
            return lux::cxx::unexpected(failure(ESceneDescriptionError::SIZE_OVERFLOW, id));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(ESceneDescriptionError::ALLOCATION_FAILURE, id));
        }
    }

    lux::cxx::expected<void, SceneDescriptionFailure> SceneDescriptionBuilder::setSystemConfiguration(
        system::SystemInstanceId id,
        std::span<const std::byte> configuration
    ) noexcept
    {
        auto found = findSystem(impl_->systems, id);
        if (found == impl_->systems.end())
        {
            return lux::cxx::unexpected(failure(ESceneDescriptionError::SYSTEM_NOT_FOUND, id));
        }
        if (found->configuration_schema_name.empty() && !configuration.empty())
        {
            return lux::cxx::unexpected(failure(ESceneDescriptionError::INVALID_CONFIGURATION_SCHEMA, id));
        }
        try
        {
            found->configuration.assign(configuration.begin(), configuration.end());
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(ESceneDescriptionError::ALLOCATION_FAILURE, id));
        }
    }

    lux::cxx::expected<void, SceneDescriptionFailure> SceneDescriptionBuilder::eraseSystem(
        system::SystemInstanceId id
    ) noexcept
    {
        auto found = findSystem(impl_->systems, id);
        if (found == impl_->systems.end())
        {
            return lux::cxx::unexpected(failure(ESceneDescriptionError::SYSTEM_NOT_FOUND, id));
        }
        impl_->systems.erase(found);
        std::erase_if(impl_->bindings, [id](const auto& binding) noexcept { return binding.system == id; });
        std::erase_if(impl_->dependencies, [id](const auto& edge) noexcept {
            return edge.before == id || edge.after == id;
        });
        return {};
    }

    lux::cxx::expected<void, SceneDescriptionFailure> SceneDescriptionBuilder::bindRequirement(
        system::SystemInstanceId system,
        std::string_view requirement,
        std::string_view provider
    ) noexcept
    {
        if (findSystem(impl_->systems, system) == impl_->systems.end())
        {
            return lux::cxx::unexpected(failure(ESceneDescriptionError::SYSTEM_NOT_FOUND, system));
        }
        if (requirement.empty())
        {
            return lux::cxx::unexpected(failure(ESceneDescriptionError::INVALID_REQUIREMENT, system));
        }
        if (provider.empty())
        {
            return lux::cxx::unexpected(failure(ESceneDescriptionError::INVALID_PROVIDER_NAME, system));
        }
        const bool duplicate = std::ranges::any_of(impl_->bindings, [&](const auto& binding) noexcept {
            return binding.system == system && binding.requirement == requirement;
        });
        if (duplicate)
        {
            return lux::cxx::unexpected(failure(ESceneDescriptionError::DUPLICATE_REQUIREMENT_BINDING, system));
        }
        try
        {
            impl_->bindings.push_back({system, std::string(requirement), std::string(provider)});
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(ESceneDescriptionError::ALLOCATION_FAILURE, system));
        }
    }

    lux::cxx::expected<void, SceneDescriptionFailure> SceneDescriptionBuilder::unbindRequirement(
        system::SystemInstanceId system,
        std::string_view requirement
    ) noexcept
    {
        const auto found = std::find_if(impl_->bindings.begin(), impl_->bindings.end(), [&](const auto& binding) noexcept {
            return binding.system == system && binding.requirement == requirement;
        });
        if (found == impl_->bindings.end())
        {
            return lux::cxx::unexpected(failure(ESceneDescriptionError::INVALID_REQUIREMENT, system));
        }
        impl_->bindings.erase(found);
        return {};
    }

    lux::cxx::expected<void, SceneDescriptionFailure> SceneDescriptionBuilder::addDependency(
        system::SystemInstanceId before,
        system::SystemInstanceId after
    ) noexcept
    {
        if (before == after || findSystem(impl_->systems, before) == impl_->systems.end() ||
            findSystem(impl_->systems, after) == impl_->systems.end())
        {
            return lux::cxx::unexpected(failure(ESceneDescriptionError::INVALID_DEPENDENCY, after));
        }
        const bool duplicate = std::ranges::any_of(impl_->dependencies, [before, after](const auto& edge) noexcept {
            return edge.before == before && edge.after == after;
        });
        if (duplicate)
        {
            return lux::cxx::unexpected(failure(ESceneDescriptionError::DUPLICATE_DEPENDENCY, after));
        }
        try
        {
            impl_->dependencies.push_back({before, after});
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(ESceneDescriptionError::ALLOCATION_FAILURE, after));
        }
    }

    lux::cxx::expected<void, SceneDescriptionFailure> SceneDescriptionBuilder::eraseDependency(
        system::SystemInstanceId before,
        system::SystemInstanceId after
    ) noexcept
    {
        const auto found = std::find_if(
            impl_->dependencies.begin(),
            impl_->dependencies.end(),
            [before, after](const auto& edge) noexcept { return edge.before == before && edge.after == after; }
        );
        if (found == impl_->dependencies.end())
        {
            return lux::cxx::unexpected(failure(ESceneDescriptionError::INVALID_DEPENDENCY, after));
        }
        impl_->dependencies.erase(found);
        return {};
    }

    void SceneDescriptionBuilder::clear() noexcept
    {
        impl_->world = {};
        impl_->simulation = {};
        impl_->systems.clear();
        impl_->bindings.clear();
        impl_->dependencies.clear();
    }

    lux::cxx::expected<SceneDescription, SceneDescriptionFailure> SceneDescriptionBuilder::build() && noexcept
    {
        if (impl_->world.isNull())
        {
            return lux::cxx::unexpected(failure(ESceneDescriptionError::INVALID_WORLD));
        }
        if (impl_->simulation.isNull())
        {
            return lux::cxx::unexpected(failure(ESceneDescriptionError::INVALID_SIMULATION));
        }
        try
        {
            std::ranges::sort(impl_->systems, [](const auto& left, const auto& right) noexcept {
                return left.id < right.id;
            });
            SceneDescription result;
            result.world_ = impl_->world;
            result.simulation_ = impl_->simulation;
            result.systems_.reserve(impl_->systems.size());
            result.system_ordinals_.reserve(impl_->systems.size());

            std::vector<system::SystemInstanceId> instances;
            instances.reserve(impl_->systems.size());
            for (const auto& source : impl_->systems)
            {
                if (source.configuration.size() > std::numeric_limits<std::size_t>::max() -
                    result.configuration_payload_.size())
                {
                    return lux::cxx::unexpected(failure(ESceneDescriptionError::SIZE_OVERFLOW, source.id));
                }
                const std::size_t offset = result.configuration_payload_.size();
                result.configuration_payload_.insert(
                    result.configuration_payload_.end(),
                    source.configuration.begin(),
                    source.configuration.end()
                );
                result.system_ordinals_.emplace(source.id.value, result.systems_.size());
                result.systems_.push_back({
                    source.id,
                    source.instance_name,
                    source.type,
                    source.version,
                    source.configuration_schema_name,
                    source.configuration_schema_name.empty()
                        ? 0U
                        : lux::cxx::Fnv1a64::hash(source.configuration_schema_name),
                    source.configuration_schema_version,
                    offset,
                    source.configuration.size()
                });
                instances.push_back(source.id);
            }

            for (const auto& binding : impl_->bindings)
            {
                result.requirement_bindings_.push_back({
                    result.system_ordinals_.at(binding.system.value),
                    binding.requirement,
                    binding.provider
                });
            }
            std::ranges::sort(result.requirement_bindings_, [](const auto& left, const auto& right) noexcept {
                return std::tie(left.system_ordinal, left.requirement, left.provider) <
                    std::tie(right.system_ordinal, right.requirement, right.provider);
            });

            std::vector<system::detail::SystemDependencyOrdinalEdge> order_edges;
            order_edges.reserve(impl_->dependencies.size());
            for (const auto& edge : impl_->dependencies)
            {
                const auto before = result.system_ordinals_.at(edge.before.value);
                const auto after = result.system_ordinals_.at(edge.after.value);
                result.dependencies_.push_back({before, after});
                order_edges.push_back({before, after});
            }
            std::ranges::sort(result.dependencies_, [](const auto& left, const auto& right) noexcept {
                return std::tie(left.before_system, left.after_system) <
                    std::tie(right.before_system, right.after_system);
            });
            const auto order = system::detail::deterministicSystemOrder(instances, order_edges);
            if (!order)
            {
                ESceneDescriptionError code{ESceneDescriptionError::INVALID_DEPENDENCY};
                if (order.error() == system::detail::ESystemDependencyOrderError::CYCLE)
                {
                    code = ESceneDescriptionError::DEPENDENCY_CYCLE;
                }
                else if (order.error() == system::detail::ESystemDependencyOrderError::ALLOCATION_FAILURE)
                {
                    code = ESceneDescriptionError::ALLOCATION_FAILURE;
                }
                return lux::cxx::unexpected(failure(code));
            }
            return result;
        }
        catch (const std::length_error&)
        {
            return lux::cxx::unexpected(failure(ESceneDescriptionError::SIZE_OVERFLOW));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(ESceneDescriptionError::ALLOCATION_FAILURE));
        }
    }
} // namespace lux::scene
