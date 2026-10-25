#pragma once

#include <cstddef>
#include <lux/cxx/compile_time/TypeToken.hpp>
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

    // Cold Editor assembly. A draft owns its decoded value; the editor only borrows it during draw.
    // Context retains provider code until all of its panes and their drafts have been destroyed.
    struct ComponentEditor final
    {
        std::shared_ptr<const void> code_lifetime;
        cxx::TypeToken type;
        void (*draw)(void*) noexcept {};
    };

    // A configuration widget owns its draft and uses the registered portable codec on serialization.
    // The factory and serializer are one provider declaration, qualified by the codec's value type.
    struct SceneConfigurationEditor final
    {
        std::shared_ptr<const void> code_lifetime;
        cxx::TypeToken type;
        std::unique_ptr<ui::Element> (*create)(EditorContext&, std::span<const std::byte>) noexcept {};
        bool (*serialize)(const ui::Element&, std::vector<std::byte>&) noexcept {};
    };
} // namespace lux::editor
