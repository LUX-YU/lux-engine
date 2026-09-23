#include <lux/engine/editor/metadata/PluginLibrary.hpp>
#include <lux/engine/dynamic_library/DynamicLibrary.hpp>
#include <lux/engine/dynamic_library/LibraryExport.hpp>

#include <algorithm>
#include <unordered_set>

namespace lux::editor
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

PluginResult<std::shared_ptr<LibraryOwner>> openLibrary(const PluginDescription &plugin,
    const PluginLibraryDescription &description, std::string_view sdk_abi,
    std::vector<std::shared_ptr<const void>> dependencies) noexcept
{
    const auto failure = [&](EPluginError code, std::string detail = {}) {
        return lux::cxx::unexpected(PluginFailure{code, plugin.identity.id, description.path.string(), std::move(detail)});
    };
    std::error_code error;
    const auto root = std::filesystem::canonical(plugin.root, error);
    if (error) return failure(EPluginError::INVALID_PATH, error.message());
    const auto path = std::filesystem::canonical(root / description.path, error);
    if (error) return failure(EPluginError::LIBRARY_LOAD_FAILURE, error.message());
    const auto relative = path.lexically_relative(root);
    const bool escapes_root = relative.empty() || relative.is_absolute() ||
        std::ranges::any_of(relative, [](const auto &part) { return part == ".."; });
    if (escapes_root) return failure(EPluginError::INVALID_PATH);
    if (description.sdk_abi != sdk_abi) return failure(EPluginError::ABI_MISMATCH);
    auto owner = std::make_shared<LibraryOwner>();
    owner->dependencies = std::move(dependencies);
    if (!owner->library.load(path, ELoadMode::INSTALLED_PLUGIN))
        return failure(EPluginError::LIBRARY_LOAD_FAILURE, owner->library.last_error());
    const auto get = owner->library.get_symbol<GetLibraryExportIdentity>(engine::platform::kLibraryIdentitySymbol);
    if (!get) return failure(EPluginError::MISSING_EXPORT, engine::platform::kLibraryIdentitySymbol);
    const auto *identity = get();
    if (!identity || identity->structure_size != sizeof(LibraryExportIdentity) || identity->interface_version != 1 ||
        !identity->module_id || !identity->sdk_abi || !identity->build_id || !identity->declaration_digest)
        return failure(EPluginError::INVALID_EXPORT);
    if (identity->module_id != plugin.identity.id || identity->module_version != plugin.identity.version)
        return failure(EPluginError::MODULE_MISMATCH);
    if (identity->sdk_abi != sdk_abi) return failure(EPluginError::ABI_MISMATCH);
    if (identity->build_id != description.build_id) return failure(EPluginError::BUILD_MISMATCH);
    if (identity->declaration_digest != description.declaration_digest) return failure(EPluginError::DECLARATION_MISMATCH);
    return owner;
}

template <class Table>
PluginResult<const Table *> exports(const DynamicLibrary &library, std::string_view plugin, const char *symbol) noexcept
{
    using Get = const Table *() noexcept;
    const auto get = library.get_symbol<Get>(symbol);
    if (!get) return lux::cxx::unexpected(PluginFailure{EPluginError::MISSING_EXPORT, std::string(plugin), symbol});
    const auto *table = get();
    const bool invalid = !table || table->structure_size != sizeof(Table) || table->interface_version != 1;
    if (invalid) return lux::cxx::unexpected(PluginFailure{EPluginError::INVALID_EXPORT, std::string(plugin), symbol});
    if constexpr (requires { table->count; table->entries; })
    {
        if (table->count > 65536 || (table->count && !table->entries))
            return lux::cxx::unexpected(PluginFailure{EPluginError::INVALID_EXPORT, std::string(plugin), symbol});
    }
    return table;
}

template <class Table, class Entry>
PluginResult<void> copyExports(const DynamicLibrary &library, std::string_view plugin, const char *symbol,
    std::vector<Entry> &output, const std::shared_ptr<const void> &owner) noexcept
{
    auto table = exports<Table>(library, plugin, symbol);
    if (!table) return lux::cxx::unexpected(table.error());
    if ((*table)->count) output.assign((*table)->entries, (*table)->entries + (*table)->count);
    if constexpr (requires(Entry &entry) { entry.code_lifetime; })
        for (auto &entry : output) entry.code_lifetime = owner;
    return {};
}
}

std::string_view pluginSdkAbi() noexcept { return LUX_PLUGIN_SDK_ABI; }

PluginLibrary::~PluginLibrary() = default;

PluginResult<std::shared_ptr<const PluginLibrary>> PluginLibrary::load(const PluginDescription &description,
    std::span<const std::shared_ptr<const PluginLibrary>> dependencies) noexcept
{
    const auto sdk_abi = pluginSdkAbi();
    auto result = std::shared_ptr<PluginLibrary>(new PluginLibrary());
    result->identity_ = description.identity;
    std::vector<std::shared_ptr<const void>> runtime_dependencies, editor_dependencies;
    for (const auto &expected : description.dependencies)
    {
        const auto found = std::ranges::find_if(dependencies, [&](const auto &plugin) {
            return plugin && plugin->identity() == expected;
        });
        if (found == dependencies.end())
            return lux::cxx::unexpected(PluginFailure{EPluginError::MISSING_DEPENDENCY, description.identity.id, expected.id});
        runtime_dependencies.push_back((*found)->runtimeCode());
        editor_dependencies.push_back((*found)->editorCode());
    }
    auto runtime = openLibrary(description, description.runtime_library, sdk_abi, std::move(runtime_dependencies));
    if (!runtime) return lux::cxx::unexpected(runtime.error());
    result->runtime_ = *runtime;
    const auto &library = (*runtime)->library;
    for (const auto kind : description.runtime_library.exports)
    {
        PluginResult<void> copied;
        switch (kind)
        {
        case EPluginExport::SIMULATION:
            copied = copyExports<simulation::SimulationPluginExports>(library, description.identity.id,
                simulation::kSimulationPluginExportsSymbol, result->simulation_, result->runtime_); break;
        case EPluginExport::SCENE:
            copied = copyExports<scene::ScenePluginExports>(library, description.identity.id,
                scene::kScenePluginExportsSymbol, result->scene_, result->runtime_); break;
        case EPluginExport::COMPONENTS:
            copied = copyExports<simulation::ecs::ComponentPluginExports>(library, description.identity.id,
                simulation::ecs::kComponentPluginExportsSymbol, result->components_, result->runtime_); break;
        case EPluginExport::RENDER:
            copied = copyExports<render::RenderPluginExports>(library, description.identity.id,
                render::kRenderPluginExportsSymbol, result->features_, result->runtime_); break;
        case EPluginExport::RENDER_SCENE:
            copied = copyExports<scene::RenderScenePluginExports>(library, description.identity.id,
                scene::kRenderScenePluginExportsSymbol, result->bindings_, result->runtime_); break;
        case EPluginExport::EDITOR:
            return lux::cxx::unexpected(PluginFailure{EPluginError::INVALID_EXPORT, description.identity.id, "editor"});
        }
        if (!copied) return lux::cxx::unexpected(copied.error());
    }
    const auto mismatch = [&](std::string subject) {
        return lux::cxx::unexpected(PluginFailure{EPluginError::DECLARATION_MISMATCH, description.identity.id, std::move(subject)});
    };
    if (result->simulation_.size() + result->scene_.size() != description.systems.size() ||
        result->components_.size() != description.components.size() ||
        result->features_.size() != description.render_features.size() ||
        result->bindings_.size() != description.render_scene_bindings.size()) return mismatch("table.count");
    simulation::SimulationSystemRegistry verified_systems;
    if (!verified_systems.add(result->simulation_)) return mismatch("simulation.contract");
    std::unordered_set<std::uint64_t> scene_types;
    for (const auto &entry : result->scene_)
        if (!scene::validSceneSystemRegistration(entry) || !scene_types.insert(entry.type.hash).second)
            return mismatch("scene.contract");
    const auto componentName = [&](lux::cxx::TypeToken type) -> std::string {
        for (const auto &entry : result->components_)
            if (entry.cpp_type == type) return std::string(entry.id.name);
        for (const auto &dependency : dependencies)
            for (const auto &entry : dependency->components())
                if (entry.cpp_type == type) return std::string(entry.id.name);
        std::string name(type.name());
        for (std::size_t p{}; (p = name.find("::", p)) != std::string::npos;) name.replace(p, 2, ".");
        return name;
    };
    for (const auto &system : description.systems)
    {
        const system::SystemTypeDescription *type{};
        bool installed{};
        if (system.domain == EMetadataSystemDomain::SIMULATION)
        {
            const auto found = std::ranges::find_if(result->simulation_, [&](const auto &entry) { return entry.type.name == system.identity.id; });
            if (found != result->simulation_.end())
            {
                type = found->description ? &found->description->type : nullptr;
                installed = found->install && found->cpp_type.isValid();
                if (system.access.size() != found->access.components.size() + found->access.external.size())
                    return mismatch(system.identity.id + ".access");
                for (const auto &access : found->access.components)
                    if (!std::ranges::any_of(system.access, [&](const auto &declared) {
                        return !declared.external && declared.type == componentName(access.type) &&
                            declared.write == (access.mode == simulation::ESystemAccessMode::WRITE);
                    })) return mismatch(system.identity.id + ".access");
                for (const auto &access : found->access.external)
                    if (!std::ranges::any_of(system.access, [&](const auto &declared) {
                        return declared.external && declared.type == componentName(access.type) &&
                            declared.write == (access.mode == simulation::ESystemAccessMode::WRITE);
                    })) return mismatch(system.identity.id + ".access");
            }
        }
        else
        {
            const auto found = std::ranges::find_if(result->scene_, [&](const auto &entry) { return entry.type.name == system.identity.id; });
            if (found != result->scene_.end())
            {
                type = found->description;
                installed = found->install && found->cpp_type.isValid();
                if (system.requirements.size() != found->requirements.size()) return mismatch(system.identity.id + ".requirements");
                for (const auto &requirement : found->requirements)
                    if (!std::ranges::any_of(system.requirements, [&](const auto &declared) {
                        return declared.slot == requirement.name && declared.ability.id == requirement.capability &&
                            declared.ability.version == 1 && declared.optional == requirement.optional;
                    })) return mismatch(system.identity.id + ".requirements");
            }
        }
        if (!installed || !type || type->version != system.identity.version) return mismatch(system.identity.id);
        if (system.configuration)
        {
            if (type->configuration_schema_name != system.configuration->id ||
                type->configuration_schema_version != system.configuration->version) return mismatch(system.identity.id);
        }
        else if (!type->configuration_schema_name.empty()) return mismatch(system.identity.id);
    }
    for (const auto &implementation : description.implementations)
    {
        std::span<const std::string_view> capabilities;
        for (const auto &entry : result->simulation_)
            if (entry.type.name == implementation.system) capabilities = entry.description->type.capabilities;
        for (const auto &entry : result->scene_)
            if (entry.type.name == implementation.system) capabilities = entry.description->capabilities;
        if (implementation.ability.version != 1 ||
            std::ranges::find(capabilities, implementation.ability.id) == capabilities.end())
            return mismatch(implementation.system + ".ability");
    }
    for (const auto &component : description.components)
    {
        const auto found = std::ranges::find_if(result->components_, [&](const auto &entry) { return entry.id.name == component.identity.id; });
        if (found == result->components_.end() || found->version != component.identity.version || !found->cpp_type.isValid() ||
            !found->operations.valid() || component.runtime_derived !=
                (found->semantic_kind == simulation::ecs::EComponentSemanticKind::RUNTIME_DERIVED))
            return mismatch(component.identity.id);
    }
    for (const auto &feature : description.render_features)
    {
        const auto found = std::ranges::find_if(result->features_, [&](const auto &entry) { return entry.factory.descriptor.canonical_name == feature.identity.id; });
        if (found == result->features_.end() ||
            found->factory.descriptor.type != render::featureId(feature.identity.id) ||
            found->factory.descriptor.abi_version != feature.identity.version || found->scene_configurable != feature.scene_configurable)
            return mismatch(feature.identity.id);
        const auto &factory = found->factory;
        const auto &contract = factory.descriptor;
        const bool invalid_factory = !factory.create_fn || !factory.name || !factory.name[0] ||
            factory.operation_count > 16 || factory.operation_count != feature.operation_count ||
            (factory.operation_count && (!factory.register_ops_fn || !factory.unregister_ops_fn));
        const bool invalid_relations = contract.dependencies.size() != feature.dependencies.size() ||
            contract.conflicts.size() != feature.conflicts.size() ||
            (contract.multiplicity == render::FeatureMultiplicity::MultiplePerScene) != feature.multiple;
        if (invalid_factory || invalid_relations) return mismatch(feature.identity.id);
        if (feature.configuration && (!found->configuration.valid() ||
            found->configuration.schema != feature.configuration->id ||
            found->configuration.schema_version != feature.configuration->version)) return mismatch(feature.identity.id);
        if (!feature.configuration && found->configuration.valid()) return mismatch(feature.identity.id);
        for (const auto &dependency : feature.dependencies)
            if (!std::ranges::any_of(contract.dependencies, [&](const auto &actual) {
                return actual.type == render::featureId(dependency.feature.id) &&
                    actual.abi_version == dependency.feature.version && actual.optional == dependency.optional;
            })) return mismatch(feature.identity.id + ".dependencies");
        for (const auto &conflict : feature.conflicts)
            if (std::ranges::find(contract.conflicts, render::featureId(conflict)) == contract.conflicts.end())
                return mismatch(feature.identity.id + ".conflicts");
    }
    for (const auto &binding : description.render_scene_bindings)
    {
        const auto found = std::ranges::find_if(result->bindings_, [&](const auto &entry) {
            return entry.feature == render::featureId(binding.feature) && entry.scene_system.name == binding.scene_system;
        });
        if (found == result->bindings_.end() || !found->create_sync_stage ||
            found->observations.size() != binding.observations.size()) return mismatch(binding.feature + ".binding");
        for (const auto &observation : found->observations)
            if (!std::ranges::any_of(binding.observations, [&](const auto &declared) {
                return declared.component == componentName(observation.component) && declared.events == observation.events;
            })) return mismatch(binding.feature + ".observations");
    }
    if (description.editor_library)
    {
        editor_dependencies.push_back(result->runtime_);
        auto editor = openLibrary(description, *description.editor_library, sdk_abi, std::move(editor_dependencies));
        if (!editor) return lux::cxx::unexpected(editor.error());
        result->editor_ = *editor;
        auto table = exports<EditorPluginExports>((*editor)->library, description.identity.id, kEditorPluginExportsSymbol);
        if (!table) return lux::cxx::unexpected(table.error());
        result->tools_ = *table;
        const bool invalid = !result->tools_->register_types || result->tools_->configuration_count > 65536 ||
            (result->tools_->configuration_count && !result->tools_->configurations);
        if (invalid) return lux::cxx::unexpected(PluginFailure{EPluginError::INVALID_EXPORT, description.identity.id, "editor"});
        std::unordered_set<std::string_view> schemas;
        for (const auto &entry : std::span{result->tools_->configurations, result->tools_->configuration_count})
        {
            const bool invalid_configuration = !entry.schema_name || !entry.schema_version || !entry.codec.valid() ||
                !entry.reflection || !entry.edit;
            if (invalid_configuration || !schemas.insert(entry.schema_name).second) return mismatch("editor.configuration");
            const auto declared = std::ranges::find_if(description.configurations, [&](const auto &configuration) {
                return configuration.identity.id == entry.schema_name && configuration.identity.version == entry.schema_version;
            });
            if (declared == description.configurations.end()) return mismatch(entry.schema_name);
        }

    }
    return std::shared_ptr<const PluginLibrary>(std::move(result));
}
} // namespace lux::editor
