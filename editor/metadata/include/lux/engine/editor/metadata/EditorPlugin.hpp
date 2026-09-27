#pragma once

#include <lux/engine/editor/metadata/EditorPluginExports.hpp>
#include <lux/engine/editor/metadata/visibility.h>
#include <lux/engine/project/PluginLibrary.hpp>

namespace lux::editor
{
    struct EditorPlugin final
    {
        project::MetadataIdentity identity;
        std::shared_ptr<const engine::platform::DynamicLibrary> code;
        const EditorPluginExports* exports{};
    };

    // Runtime modules are already verified. Only the Editor product calls this
    // boundary; a game never opens or interprets these optional exports.
    [[nodiscard]] LUX_EDITOR_METADATA_PUBLIC project::PluginResult<EditorPlugin> loadEditorPlugin(
        const project::PluginDescription& description,
        const project::PluginLibrary& runtime,
        std::span<const EditorPlugin> dependencies = {}
    ) noexcept;
}
