#include <lux/engine/dynamic_library/DynamicLibrary.hpp>
#include <lux/engine/dynamic_library/LibraryExport.hpp>
#include <lux/engine/plugins/PluginLibrary.hpp>

#include <algorithm>
#include <exception>
#include <lux/engine/plugins/detail/PluginExports.hpp>
#include <new>
#include <unordered_set>

namespace lux::plugins
{
    namespace
    {
        using engine::platform::DynamicLibrary;
        using engine::platform::ELoadMode;
        using engine::platform::GetLibraryExportIdentity;
        using engine::platform::LibraryExportIdentity;

        struct LibraryOwner final
        {
            std::vector<std::shared_ptr<const void>> dependencies;
            DynamicLibrary library;
        };

        PluginResult<std::shared_ptr<LibraryOwner>> openLibrary(
            const PluginDescription& plugin,
            const PluginLibraryDescription& description,
            std::string_view sdk_abi,
            std::vector<std::shared_ptr<const void>> dependencies
        )
        {
            const auto failure = [&](EPluginError code, std::string detail = {})
            {
                return lux::cxx::unexpected(
                    PluginFailure{code, plugin.identity.id, description.path.string(), std::move(detail)}
                );
            };
            std::error_code error;
            const auto root = std::filesystem::canonical(plugin.root, error);
            if (error)
            {
                return failure(EPluginError::INVALID_PATH, error.message());
            }
            const auto path = std::filesystem::canonical(root / description.path, error);
            if (error)
            {
                return failure(EPluginError::LIBRARY_LOAD_FAILURE, error.message());
            }
            const auto relative = path.lexically_relative(root);
            const bool escapes_root = relative.empty() || relative.is_absolute() ||
                                      std::ranges::any_of(relative, [](const auto& part) { return part == ".."; });
            if (escapes_root)
            {
                return failure(EPluginError::INVALID_PATH);
            }
            if (description.sdk_abi != sdk_abi)
            {
                return failure(EPluginError::ABI_MISMATCH);
            }
            auto owner = std::make_shared<LibraryOwner>();
            owner->dependencies = std::move(dependencies);
            if (!owner->library.load(path, ELoadMode::INSTALLED_PLUGIN))
            {
                return failure(EPluginError::LIBRARY_LOAD_FAILURE, owner->library.last_error());
            }
            const auto get =
                owner->library.get_symbol<GetLibraryExportIdentity>(engine::platform::kLibraryIdentitySymbol);
            if (!get)
            {
                return failure(EPluginError::MISSING_EXPORT, engine::platform::kLibraryIdentitySymbol);
            }
            const auto* identity = get();
            if (!identity || identity->structure_size != sizeof(LibraryExportIdentity) ||
                identity->interface_version != 1 || !identity->module_id || !identity->sdk_abi || !identity->build_id ||
                !identity->declaration_digest)
            {
                return failure(EPluginError::INVALID_EXPORT);
            }
            if (identity->module_id != plugin.identity.id || identity->module_version != plugin.identity.version)
            {
                return failure(EPluginError::MODULE_MISMATCH);
            }
            if (identity->sdk_abi != sdk_abi)
            {
                return failure(EPluginError::ABI_MISMATCH);
            }
            if (identity->build_id != description.build_id)
            {
                return failure(EPluginError::BUILD_MISMATCH);
            }
            if (identity->declaration_digest != description.declaration_digest)
            {
                return failure(EPluginError::DECLARATION_MISMATCH);
            }
            return owner;
        }

    } // namespace

    std::string_view pluginSdkAbi() noexcept
    {
        return LUX_PLUGIN_SDK_ABI;
    }

    PluginResult<std::shared_ptr<const engine::platform::DynamicLibrary>> loadPluginLibrary(
        const PluginDescription& plugin,
        const PluginLibraryDescription& description,
        std::span<const std::shared_ptr<const void>> dependencies
    ) noexcept
    try
    {
        auto loaded = openLibrary(plugin, description, pluginSdkAbi(), {dependencies.begin(), dependencies.end()});
        if (!loaded)
        {
            return lux::cxx::unexpected(loaded.error());
        }
        return std::shared_ptr<const engine::platform::DynamicLibrary>(*loaded, &(*loaded)->library);
    }

    catch (const std::bad_alloc&)
    {
        std::terminate();
    }
    catch (...)
    {
        return lux::cxx::unexpected(PluginFailure{EPluginError::LIBRARY_LOAD_FAILURE, {}, {}, "Plugin admission failed"}
        );
    }

    PluginLibrary::~PluginLibrary() = default;

    PluginResult<std::shared_ptr<const PluginLibrary>> PluginLibrary::load(
        const PluginDescription& description,
        std::span<const std::shared_ptr<const PluginLibrary>> dependencies
    ) noexcept
    try
    {
        const auto sdk_abi = pluginSdkAbi();
        auto result = std::shared_ptr<PluginLibrary>(new PluginLibrary());
        result->description_ = description;
        std::vector<std::shared_ptr<const void>> runtime_dependencies;
        for (const auto& expected : description.dependencies)
        {
            const auto found = std::ranges::find_if(
                dependencies,
                [&](const auto& plugin) { return plugin && plugin->identity() == expected; }
            );
            if (found == dependencies.end())
            {
                return lux::cxx::unexpected(
                    PluginFailure{EPluginError::MISSING_DEPENDENCY, description.identity.id, expected.id}
                );
            }
            runtime_dependencies.push_back((*found)->runtimeCode());
        }
        auto runtime = openLibrary(description, description.runtime_library, sdk_abi, std::move(runtime_dependencies));
        if (!runtime)
        {
            return lux::cxx::unexpected(runtime.error());
        }
        result->runtime_ = std::shared_ptr<const DynamicLibrary>(*runtime, &(*runtime)->library);
        const auto& library = (*runtime)->library;
        for (const auto kind : description.runtime_library.exports)
        {
            PluginResult<void> copied;
            switch (kind)
            {
            case EPluginExport::SIMULATION:
                copied = detail::copyExports<simulation::SimulationPluginExports>(
                    library,
                    description.identity.id,
                    simulation::kSimulationPluginExportsSymbol,
                    result->simulation_,
                    result->runtime_
                );
                break;
            case EPluginExport::SCENE:
            {
                auto table = detail::exports<scene::ScenePluginExports>(
                    library,
                    description.identity.id,
                    scene::kScenePluginExportsSymbol,
                    3
                );
                if (!table)
                {
                    return cxx::unexpected(table.error());
                }
                const auto& entries = **table;
                const bool invalid_geometry_range =
                    entries.geometry_count > 65536 || (entries.geometry_count && !entries.geometries);
                const bool invalid_partition_range =
                    entries.partition_count > 65536 || (entries.partition_count && !entries.partitions);
                if (invalid_geometry_range || invalid_partition_range)
                {
                    return cxx::unexpected(PluginFailure{EPluginError::INVALID_EXPORT, description.identity.id});
                }
                if (entries.count)
                {
                    result->scene_.assign(entries.entries, entries.entries + entries.count);
                }
                if (entries.geometry_count)
                {
                    result->geometries_.assign(entries.geometries, entries.geometries + entries.geometry_count);
                }
                if (entries.partition_count)
                {
                    result->partitions_.assign(entries.partitions, entries.partitions + entries.partition_count);
                }
                for (auto& entry : result->scene_)
                {
                    entry.code_lifetime = result->runtime_;
                }
                for (auto& entry : result->geometries_)
                {
                    if (!scene::validGeometryDeclaration(entry))
                    {
                        return cxx::unexpected(PluginFailure{EPluginError::INVALID_EXPORT, description.identity.id});
                    }
                    entry.code_lifetime = result->runtime_;
                }
                for (auto& entry : result->partitions_)
                {
                    if (!world::validPartitionDeclaration(entry))
                    {
                        return cxx::unexpected(PluginFailure{EPluginError::INVALID_EXPORT, description.identity.id});
                    }
                    entry.code_lifetime = result->runtime_;
                }
                break;
            }
            case EPluginExport::COMPONENTS:
                copied = detail::copyExports<ecs::ComponentPluginExports>(
                    library,
                    description.identity.id,
                    ecs::kComponentPluginExportsSymbol,
                    result->components_,
                    result->runtime_,
                    2
                );
                break;
            case EPluginExport::RENDER:
            case EPluginExport::RENDER_SCENE:
                // Graphics tables are interpreted only by the optional rendering adapter.
                break;
            case EPluginExport::EDITOR:
                return lux::cxx::unexpected(
                    PluginFailure{EPluginError::INVALID_EXPORT, description.identity.id, "editor"}
                );
            }
            if (!copied)
            {
                return lux::cxx::unexpected(copied.error());
            }
        }
        const auto mismatch = [&](std::string subject)
        {
            return lux::cxx::unexpected(
                PluginFailure{EPluginError::DECLARATION_MISMATCH, description.identity.id, std::move(subject)}
            );
        };
        if (result->simulation_.size() + result->scene_.size() != description.systems.size() ||
            result->components_.size() != description.components.size())
        {
            return mismatch("table.count");
        }
        if (result->geometries_.size() != description.geometries.size() ||
            result->partitions_.size() != description.partitions.size())
        {
            return mismatch("content.count");
        }
        for (const auto& geometry : result->geometries_)
        {
            const auto declared = std::ranges::find(
                description.geometries,
                geometry.name,
                [](const auto& item) -> const std::string& { return item.identity.id; }
            );
            const bool missing = declared == description.geometries.end();
            if (missing || declared->identity.version != geometry.version ||
                declared->partition_input_type != geometry.partition_input_type.name() ||
                declared->components.size() != geometry.components.size() ||
                declared->data.size() != geometry.data.size())
            {
                return mismatch("geometry.contract");
            }
            for (const auto& component : geometry.components)
            {
                if (std::ranges::find(
                        declared->components,
                        MetadataIdentity{component.schema.name, component.version}
                    ) == declared->components.end())
                {
                    return mismatch("geometry.schema");
                }
            }
            for (const auto& contract : geometry.data)
            {
                const auto same = [&](const auto& item)
                {
                    return item.identity.id == contract.name && item.identity.version == contract.version &&
                           item.prepared == (contract.kind == system::ESystemDataKind::PREPARED_DATA) &&
                           item.phase == static_cast<std::uint8_t>(contract.phase);
                };
                if (!std::ranges::any_of(declared->data, same))
                {
                    return mismatch("geometry.data");
                }
            }
        }
        for (const auto& partition : result->partitions_)
        {
            const auto declared = std::ranges::find(
                description.partitions,
                partition.identity.id.name,
                [](const auto& item) -> const std::string& { return item.identity.id; }
            );
            const bool missing = declared == description.partitions.end();
            if (missing || declared->identity.version != partition.identity.version ||
                declared->input_type != partition.input_type.name() ||
                declared->configuration_type != partition.configuration.type.name() ||
                declared->configuration_version != partition.configuration_version ||
                declared->constructs_empty_world != (partition.build_empty != nullptr) ||
                declared->queries.size() != partition.queries.size())
            {
                return mismatch("partition.contract");
            }
            for (const auto& query : partition.queries)
            {
                if (std::ranges::find(declared->queries, MetadataIdentity{query.type.name, query.version}) ==
                    declared->queries.end())
                {
                    return mismatch("partition.query");
                }
            }
        }
        simulation::SimulationSystemRegistry verified_systems;
        if (!verified_systems.add(result->simulation_))
        {
            return mismatch("simulation.contract");
        }
        std::unordered_set<std::uint64_t> scene_types;
        for (const auto& entry : result->scene_)
        {
            if (!scene::validSceneSystemRegistration(entry) || !scene_types.insert(entry.type.hash).second)
            {
                return mismatch("scene.contract");
            }
        }
        const auto componentName = [&](lux::cxx::type_info type) -> std::string
        {
            for (const auto& entry : result->components_)
            {
                if (entry.cpp_type == type)
                {
                    return std::string(entry.id.name);
                }
            }
            for (const auto& dependency : dependencies)
            {
                for (const auto& entry : dependency->components())
                {
                    if (entry.cpp_type == type)
                    {
                        return std::string(entry.id.name);
                    }
                }
            }
            std::string name(type.name());
            for (std::size_t p{}; (p = name.find("::", p)) != std::string::npos;)
            {
                name.replace(p, 2, ".");
            }
            return name;
        };
        for (const auto& system : description.systems)
        {
            const system::SystemTypeDescription* type{};
            bool installed{};
            if (system.domain == EMetadataSystemDomain::SIMULATION)
            {
                const auto found = std::ranges::find_if(
                    result->simulation_,
                    [&](const auto& entry) { return entry.type.name == system.identity.id; }
                );
                if (found != result->simulation_.end())
                {
                    type = found->description ? &found->description->type : nullptr;
                    installed = found->install && found->cpp_type.isValid();
                    if (system.access.size() != found->access.components.size() + found->access.external.size())
                    {
                        return mismatch(system.identity.id + ".access");
                    }
                    for (const auto& access : found->access.components)
                    {
                        if (!std::ranges::any_of(
                                system.access,
                                [&](const auto& declared)
                                {
                                    return !declared.external && declared.type == componentName(access.type) &&
                                           declared.write == (access.mode == simulation::ESystemAccessMode::WRITE);
                                }
                            ))
                        {
                            return mismatch(system.identity.id + ".access");
                        }
                    }
                    for (const auto& access : found->access.external)
                    {
                        if (!std::ranges::any_of(
                                system.access,
                                [&](const auto& declared)
                                {
                                    return declared.external && declared.type == componentName(access.type) &&
                                           declared.write == (access.mode == simulation::ESystemAccessMode::WRITE);
                                }
                            ))
                        {
                            return mismatch(system.identity.id + ".access");
                        }
                    }
                }
            }
            else
            {
                const auto found = std::ranges::find_if(
                    result->scene_,
                    [&](const auto& entry) { return entry.type.name == system.identity.id; }
                );
                if (found != result->scene_.end())
                {
                    type = found->description;
                    installed = found->install && found->cpp_type.isValid();
                    const auto matches_requirements = [](const auto& declared, const auto& actual)
                    {
                        return declared.size() == actual.size() &&
                               std::ranges::all_of(
                                   actual,
                                   [&](const auto& requirement)
                                   {
                                       return std::ranges::any_of(
                                           declared,
                                           [&](const auto& value)
                                           {
                                               return value.slot == requirement.name &&
                                                      value.ability.id == requirement.capability &&
                                                      value.ability.version == 1 &&
                                                      value.optional == requirement.optional;
                                           }
                                       );
                                   }
                               );
                    };
                    if (!matches_requirements(system.requirements, found->requirements) ||
                        !matches_requirements(system.preparation_requirements, found->preparation_requirements))
                    {
                        return mismatch(system.identity.id + ".requirements");
                    }
                }
            }
            if (!installed || !type || type->version != system.identity.version)
            {
                return mismatch(system.identity.id);
            }
            const auto matches_data = [](const auto& declared, const auto& actual)
            {
                return declared.size() == actual.size() &&
                       std::ranges::all_of(
                           actual,
                           [&](const auto& value)
                           {
                               return std::ranges::any_of(
                                   declared,
                                   [&](const auto& record)
                                   {
                                       return record.identity.id == value.name &&
                                              record.identity.version == value.version &&
                                              record.prepared ==
                                                  (value.kind == system::ESystemDataKind::PREPARED_DATA) &&
                                              record.phase == static_cast<std::uint8_t>(value.phase);
                                   }
                               );
                           }
                       );
            };
            const bool is_invalid_declaration = !system::validSystemTypeDescription(*type);
            const bool is_data_mismatch = !matches_data(system.data_inputs, type->data_inputs) ||
                                          !matches_data(system.data_outputs, type->data_outputs);
            if (is_invalid_declaration || is_data_mismatch)
            {
                return mismatch(system.identity.id + ".data_contracts");
            }
            if (system.configuration)
            {
                if (type->configuration_schema_name != system.configuration->id ||
                    type->configuration_schema_version != system.configuration->version)
                {
                    return mismatch(system.identity.id);
                }
            }
            else if (!type->configuration_schema_name.empty())
            {
                return mismatch(system.identity.id);
            }
        }
        for (const auto& implementation : description.implementations)
        {
            std::span<const std::string_view> capabilities;
            for (const auto& entry : result->simulation_)
            {
                if (entry.type.name == implementation.system)
                {
                    capabilities = entry.description->type.capabilities;
                }
            }
            for (const auto& entry : result->scene_)
            {
                if (entry.type.name == implementation.system)
                {
                    capabilities = entry.description->capabilities;
                }
            }
            if (implementation.ability.version != 1 ||
                std::ranges::find(capabilities, implementation.ability.id) == capabilities.end())
            {
                return mismatch(implementation.system + ".ability");
            }
        }
        for (const auto& component : description.components)
        {
            const auto found = std::ranges::find_if(
                result->components_,
                [&](const auto& entry) { return entry.id.name == component.identity.id; }
            );
            if (found == result->components_.end() || found->version != component.identity.version ||
                !found->cpp_type.isValid() || !found->operations.valid() ||
                component.runtime_derived !=
                    (found->semantic_kind == ecs::EComponentSemanticKind::RUNTIME_DERIVED))
            {
                return mismatch(component.identity.id);
            }
        }
        return std::shared_ptr<const PluginLibrary>(std::move(result));
    }
    catch (const std::bad_alloc&)
    {
        std::terminate();
    }
    catch (...)
    {
        return lux::cxx::unexpected(PluginFailure{EPluginError::LIBRARY_LOAD_FAILURE, {}, {}, "Plugin admission failed"}
        );
    }
} // namespace lux::plugins
