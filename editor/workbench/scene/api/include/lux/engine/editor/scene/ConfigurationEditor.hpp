#pragma once
#include <lux/engine/editor/configuration/ConfigurationValue.hpp>
#include <lux/engine/ui/Ids.hpp>
#include <memory>
#include <lux/engine/editor/contracts/CodeLease.hpp>
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
        contracts::CodeLease code{contracts::CodeLease::builtin()};
        ConfigurationDescriptor value;
        Create create{};
    };
} // namespace lux::editor::scene

namespace lux::editor::settings
{
    class SettingsEntry;
    // Optional configuration control factory. The immutable entry pins the code implementing it;
    // a page owner must destroy its controls before releasing this record.
    struct SettingsPage final
    {
        std::shared_ptr<SettingsEntry> entry;
        scene::ConfigurationEditor::Create create{};
    };
} // namespace lux::editor::settings
