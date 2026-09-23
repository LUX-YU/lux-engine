#pragma once

#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/serialization/PortableValueCodec.hpp>

#include <cstdint>

namespace lux::ui { class Frame; }

namespace lux::editor
{
struct ConfigurationEditorRegistration final
{
    const char *schema_name;
    std::uint32_t schema_version;
    serialization::PortableValueCodec codec;
    // The class belongs to the unpublished draft until the whole plugin is adopted.
    const meta::RefClass *(*reflection)(meta::ReflectionRegistry &) noexcept;
    bool (*edit)(lux::ui::Frame &, void *) noexcept;
};

struct EditorPluginExports final
{
    std::uint32_t structure_size{sizeof(EditorPluginExports)};
    std::uint32_t interface_version{1};
    meta::ReflectionRegistrationDraft::RegisterFn register_types{};
    const ConfigurationEditorRegistration *configurations{};
    std::uint32_t configuration_count{};
};
using GetEditorPluginExports = const EditorPluginExports *() noexcept;
inline constexpr const char *kEditorPluginExportsSymbol = "lux_editor_exports_v1";
} // namespace lux::editor
