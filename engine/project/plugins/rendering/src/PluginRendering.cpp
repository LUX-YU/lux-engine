#include <lux/engine/project/PluginRendering.hpp>
#include <lux/engine/project/detail/PluginExports.hpp>
#include <algorithm>
#include <exception>
#include <new>

namespace lux::project
{
    PluginResult<PluginRenderRegistrations> readPluginRendering(
        const PluginLibrary& plugin,
        std::span<const std::shared_ptr<const PluginLibrary>> dependencies
    ) noexcept
    try
    {
        const auto& description = plugin.description();
        const auto mismatch = [&](std::string subject) {
            return lux::cxx::unexpected(
                PluginFailure{EPluginError::DECLARATION_MISMATCH, description.identity.id, std::move(subject)}
            );
        };
        PluginRenderRegistrations result;
        for (const auto kind : description.runtime_library.exports)
        {
            PluginResult<void> copied;
            if (kind == EPluginExport::RENDER)
                copied = detail::copyExports<render::RenderPluginExports>(
                    plugin.library(),
                    plugin.identity().id,
                    render::kRenderPluginExportsSymbol,
                    result.features,
                    plugin.runtimeCode()
                );
            else if (kind == EPluginExport::RENDER_SCENE)
                copied = detail::copyExports<scene::RenderScenePluginExports>(
                    plugin.library(),
                    plugin.identity().id,
                    scene::kRenderScenePluginExportsSymbol,
                    result.bindings,
                    plugin.runtimeCode()
                );
            if (!copied)
                return lux::cxx::unexpected(copied.error());
        }
        if (result.features.size() != description.render_features.size() ||
            result.bindings.size() != description.render_scene_bindings.size())
            return mismatch("table.count");
        const auto componentName = [&](lux::cxx::TypeToken type) -> std::string {
            for (const auto& entry : plugin.components())
                if (entry.cpp_type == type)
                    return std::string(entry.id.name);
            for (const auto& dependency : dependencies)
                for (const auto& entry : dependency->components())
                    if (entry.cpp_type == type)
                        return std::string(entry.id.name);
            std::string name(type.name());
            for (std::size_t p{}; (p = name.find("::", p)) != std::string::npos;)
                name.replace(p, 2, ".");
            return name;
        };
        for (const auto& feature : description.render_features)
        {
            const auto found = std::ranges::find_if(result.features, [&](const auto& entry) {
                return entry.factory.descriptor.canonical_name == feature.identity.id;
            });
            if (found == result.features.end() ||
                found->factory.descriptor.type != render::featureId(feature.identity.id) ||
                found->factory.descriptor.abi_version != feature.identity.version ||
                found->scene_configurable != feature.scene_configurable)
                return mismatch(feature.identity.id);
            const auto& factory = found->factory;
            const auto& contract = factory.descriptor;
            const bool invalid_factory =
                !factory.create_fn || !factory.name || !factory.name[0] || factory.operation_count > 16 ||
                factory.operation_count != feature.operation_count ||
                (factory.operation_count && (!factory.register_ops_fn || !factory.unregister_ops_fn));
            const bool invalid_relations =
                contract.dependencies.size() != feature.dependencies.size() ||
                contract.conflicts.size() != feature.conflicts.size() ||
                (contract.multiplicity == render::EFeatureMultiplicity::MULTIPLE_PER_SCENE) != feature.multiple;
            if (invalid_factory || invalid_relations)
                return mismatch(feature.identity.id);
            if (feature.configuration &&
                (!found->configuration.valid() || found->configuration.schema != feature.configuration->id ||
                 found->configuration.schema_version != feature.configuration->version))
                return mismatch(feature.identity.id);
            if (!feature.configuration && found->configuration.valid())
                return mismatch(feature.identity.id);
            for (const auto& dependency : feature.dependencies)
                if (!std::ranges::any_of(contract.dependencies, [&](const auto& actual) {
                        return actual.type == render::featureId(dependency.feature.id) &&
                               actual.abi_version == dependency.feature.version &&
                               actual.optional == dependency.optional;
                    }))
                    return mismatch(feature.identity.id + ".dependencies");
            for (const auto& conflict : feature.conflicts)
                if (std::ranges::find(contract.conflicts, render::featureId(conflict)) == contract.conflicts.end())
                    return mismatch(feature.identity.id + ".conflicts");
        }
        for (const auto& binding : description.render_scene_bindings)
        {
            const auto found = std::ranges::find_if(result.bindings, [&](const auto& entry) {
                return entry.feature == render::featureId(binding.feature) &&
                       entry.scene_system.name == binding.scene_system;
            });
            if (found == result.bindings.end() || !found->create_sync_stage ||
                found->observations.size() != binding.observations.size())
                return mismatch(binding.feature + ".binding");
            for (const auto& observation : found->observations)
                if (!std::ranges::any_of(binding.observations, [&](const auto& declared) {
                        return declared.component == componentName(observation.component) &&
                               declared.events == observation.events;
                    }))
                    return mismatch(binding.feature + ".observations");
        }
        return result;
    }
    catch (const std::bad_alloc&)
    {
        std::terminate();
    }
    catch (...)
    {
        return lux::cxx::unexpected(PluginFailure{EPluginError::REGISTRATION_FAILURE, {}, {}, "render admission failed"}
        );
    }
}
