#pragma once

#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/editor/metadata/CommandRegistration.hpp>
#include <lux/engine/editor/metadata/ComponentEditorRegistry.hpp>
#include <lux/engine/editor/metadata/AssetEditorRegistration.hpp>
#include <lux/engine/serialization/PortableValueCodec.hpp>

#include <cstdint>

namespace lux::editor
{
    class ConfigurationValue;
    struct ConfigurationEditorRegistration final
    {
        using CreateResult = EditorResult<std::unique_ptr<lux::ui::Element>>;
        using CreateFn =
            CreateResult (*)(lux::ui::Element&, lux::cxx::StableNameId<lux::ui::ElementIdTag>, ConfigurationValue&) noexcept;
        const char* schema_name;
        std::uint32_t schema_version;
        serialization::PortableValueCodec codec;
        // The class belongs to the unpublished draft until the whole plugin is adopted.
        const meta::RefClass* (*reflection)(meta::ReflectionRegistry&) noexcept;
        CreateFn create;
        std::shared_ptr<const void> code_lifetime;
    };

    inline constexpr std::uint32_t kEditorPluginInterfaceVersion = 6;

    struct EditorPluginExports final
    {
        std::uint32_t structure_size{sizeof(EditorPluginExports)};
        std::uint32_t interface_version{kEditorPluginInterfaceVersion};
        meta::ReflectionRegistrationDraft::RegisterFn register_types{};
        const ConfigurationEditorRegistration* configurations{};
        std::uint32_t configuration_count{};
        const ComponentEditorRegistration* component_editors{};
        std::uint32_t component_editor_count{};
        const PaneRegistration* panes{};
        std::uint32_t pane_count{};
        const AssetEditorRegistration* asset_editors{};
        std::uint32_t asset_editor_count{};
        const CommandRegistration* commands{};
        std::uint32_t command_count{};
    };
    using GetEditorPluginExports = const EditorPluginExports*() noexcept;
    inline constexpr const char* kEditorPluginExportsSymbol = "lux_editor_exports_v6";
} // namespace lux::editor
