#pragma once
#include <lux/engine/editor/configuration/ConfigurationValue.hpp>
#include <lux/engine/ui/Ids.hpp>
#include <memory>
#include <lux/engine/object/CodeLease.hpp>
#include <lux/engine/editor/EditorError.hpp>

namespace lux::ui
{
    class Element;
}

namespace lux::editor::scene
{
    struct ConfigurationEditor final
    {
        using CreateResult = EditorResult<std::unique_ptr<lux::ui::Element>>;
        using Create = CreateResult (*)(lux::ui::Element&, lux::ui::ElementId, ConfigurationValue&) noexcept;
        lux::object::CodeLease code{lux::object::CodeLease::builtin()};
        ConfigurationDescriptor value;
        Create create{};
    };
} // namespace lux::editor::scene
