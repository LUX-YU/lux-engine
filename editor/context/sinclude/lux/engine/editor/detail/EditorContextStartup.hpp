#pragma once

#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/editor/metadata/ComponentEditorRegistry.hpp>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/render/RenderRuntime.hpp>

namespace lux::editor::detail
{
    // Product-only cold inputs. Nothing here is a callback to a tool or a UI owner.
    struct EditorContextCreateInfo final
    {
        std::filesystem::path project_file;
        std::filesystem::path plugin_root;
        std::vector<render::RenderFeatureRegistration> product_features;
        std::vector<ComponentEditorRegistration> product_editors;
    };

    struct EditorContextAccess final
    {
        // A failed candidate releases its resources through the same RAII path.
        [[nodiscard]] static EditorResult<void> create(
            std::unique_ptr<EditorContext>&,
            lux::ui::Root&,
            engine::EngineContext&,
            object::ObjectMessageQueue&,
            EditorContextCreateInfo
        ) noexcept;
    };
}
