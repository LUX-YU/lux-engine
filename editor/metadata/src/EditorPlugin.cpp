#include <lux/engine/editor/metadata/EditorPlugin.hpp>
#include <lux/engine/dynamic_library/DynamicLibrary.hpp>

#include <algorithm>
#include <unordered_set>

namespace lux::editor
{
    lux::project::PluginResult<EditorPlugin> loadEditorPlugin(
        const lux::project::PluginDescription& description,
        const lux::project::PluginLibrary& runtime,
        std::span<const EditorPlugin> dependencies
    ) noexcept
    {
        const auto failure = [&](lux::project::EPluginError code, std::string subject) {
            return lux::cxx::unexpected(lux::project::PluginFailure{code, description.identity.id, std::move(subject)});
        };
        if (runtime.identity() != description.identity)
            return failure(lux::project::EPluginError::MODULE_MISMATCH, "runtime");
        EditorPlugin result{description.identity};
        if (!description.editor_library)
            return result;
        std::vector<std::shared_ptr<const void>> pins{runtime.runtimeCode()};
        for (const auto& identity : description.dependencies)
        {
            const auto found = std::ranges::find(dependencies, identity, &EditorPlugin::identity);
            if (found == dependencies.end())
                return failure(lux::project::EPluginError::MISSING_DEPENDENCY, identity.id);
            if (found->code)
                pins.push_back(found->code);
        }
        auto library = lux::project::loadPluginLibrary(description, *description.editor_library, pins);
        if (!library)
            return lux::cxx::unexpected(library.error());
        const auto get = (*library)->get_symbol<GetEditorPluginExports>(kEditorPluginExportsSymbol);
        if (!get)
            return failure(lux::project::EPluginError::MISSING_EXPORT, kEditorPluginExportsSymbol);
        const auto* table = get();
        const bool invalid_header = !table || table->structure_size != sizeof(EditorPluginExports) ||
                                    table->interface_version != kEditorPluginInterfaceVersion;
        if (invalid_header)
            return failure(lux::project::EPluginError::INVALID_EXPORT, "editor.header");
        const bool invalid_table =
            !table->register_types || table->configuration_count > 65536 ||
            (table->configuration_count && !table->configurations) || table->component_editor_count > 65536 ||
            (table->component_editor_count && !table->component_editors) || table->pane_count > 4096 ||
            (table->pane_count && !table->panes) || table->command_count > 4096 ||
            (table->command_count && !table->commands) || table->asset_editor_count > 4096 ||
            (table->asset_editor_count && !table->asset_editors);
        if (invalid_table)
            return failure(lux::project::EPluginError::INVALID_EXPORT, "editor.configurations");
        std::unordered_set<std::string_view> schemas;
        for (const auto& entry : std::span{table->configurations, table->configuration_count})
        {
            const bool invalid_configuration = !entry.schema_name || !entry.schema_version || !entry.codec.valid() ||
                                               !entry.reflection || !entry.create;
            if (invalid_configuration)
                return failure(lux::project::EPluginError::INVALID_EXPORT, "editor.configuration");
            if (!schemas.insert(entry.schema_name).second)
                return failure(lux::project::EPluginError::DECLARATION_MISMATCH, entry.schema_name);
            const bool declared = std::ranges::any_of(description.configurations, [&](const auto& configuration) {
                return configuration.identity.id == entry.schema_name &&
                       configuration.identity.version == entry.schema_version;
            });
            if (!declared)
                return failure(lux::project::EPluginError::DECLARATION_MISMATCH, entry.schema_name);
        }
        std::unordered_set<std::uint64_t> components;
        for (const auto& entry : std::span{table->component_editors, table->component_editor_count})
        {
            const bool invalid =
                !entry.create || entry.name.empty() || entry.type.hash() == 0 || entry.provider != description.identity;
            if (invalid || !components.insert(entry.type.hash()).second)
                return failure(lux::project::EPluginError::INVALID_EXPORT, "editor.components");
        }
        result.code = std::move(*library);
        for (const auto& entry : std::span{table->panes, table->pane_count})
            if (!entry.valid())
                return failure(lux::project::EPluginError::INVALID_EXPORT, "editor.tools");
        for (const auto& entry : std::span{table->asset_editors, table->asset_editor_count})
            if (!entry.valid())
                return failure(lux::project::EPluginError::INVALID_EXPORT, "editor.assets");
        for (const auto& entry : std::span{table->commands, table->command_count})
            if (!entry.valid())
                return failure(lux::project::EPluginError::INVALID_EXPORT, "editor.commands");
        result.exports = table;
        return result;
    }
}
