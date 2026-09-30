#pragma once
#include <lux/engine/editor/metadata/visibility.h>

#include <lux/engine/editor/metadata/EditorPluginExports.hpp>
#include <lux/engine/project/PluginCatalog.hpp>
#include <lux/engine/meta/RuntimeObject.hpp>

namespace lux::editor
{
    class LUX_EDITOR_METADATA_PUBLIC ConfigurationValue final
    {
    public:
        [[nodiscard]] static lux::project::PluginResult<ConfigurationValue> create(
            const ConfigurationEditorRegistration&,
            std::shared_ptr<const void> code
        ) noexcept;
        ConfigurationValue(ConfigurationValue&&) noexcept = default;
        ConfigurationValue& operator=(ConfigurationValue&&) noexcept;
        ConfigurationValue(const ConfigurationValue&) = delete;
        ConfigurationValue& operator=(const ConfigurationValue&) = delete;
        [[nodiscard]] void* data() noexcept
        {
            return value_.data();
        }
        [[nodiscard]] EditorResult<std::unique_ptr<lux::ui::Element>>
        createElement(lux::ui::Element&, lux::cxx::StableNameId<lux::ui::ElementIdTag>) noexcept;
        [[nodiscard]] serialization::SerializationResult encode(std::vector<std::byte>&) const noexcept;
        [[nodiscard]] serialization::SerializationResult decode(std::span<const std::byte>) noexcept;

    private:
        ConfigurationValue(std::shared_ptr<const void>, meta::RuntimeObject, ConfigurationEditorRegistration) noexcept;
        std::shared_ptr<const void> code_;
        meta::RuntimeObject value_;
        ConfigurationEditorRegistration registration_;
    };
} // namespace lux::editor
