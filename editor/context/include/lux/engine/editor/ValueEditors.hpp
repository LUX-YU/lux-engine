#pragma once

#include <cstddef>
#include <lux/cxx/compile_time/type_info.hpp>
#include <memory>
#include <span>
#include <vector>

namespace lux::ui
{
    class Element;
}

namespace lux::editor
{
    class EditorContext;
    class SceneConfigurationForm;

    // Cold Editor assembly. A draft owns its decoded value; the editor only borrows it during draw.
    // Context retains provider code until all of its panes and their drafts have been destroyed.
    struct ComponentEditor final
    {
        std::shared_ptr<const void> code_lifetime;
        cxx::type_info type;
        void (*draw)(void*) noexcept {};
    };

    // A configuration widget owns its draft and uses the registered portable codec on serialization.
    // The factory is qualified by the codec's value type; the form owns editing and serialization.
    struct SceneConfigurationEditor final
    {
        std::shared_ptr<const void> code_lifetime;
        cxx::type_info type;
        std::unique_ptr<SceneConfigurationForm> (*create)(EditorContext&, std::span<const std::byte>) noexcept {};
    };
} // namespace lux::editor
