#pragma once
#include <lux/engine/editor/configuration/visibility.h>
#include <lux/engine/meta/RuntimeObject.hpp>
#include <lux/engine/serialization/PortableValueCodec.hpp>
namespace lux::editor
{
    namespace settings { class SettingsEntry; }
    struct ConfigurationDescriptor final
    {
        std::string schema_name;
        std::uint32_t schema_version{};
        serialization::PortableValueCodec codec;
        const meta::RefClass* (*reflection)(meta::ReflectionRegistry&) noexcept {};
    };
    enum class EConfigurationError : std::uint8_t
    {
        INVALID_DESCRIPTOR,
        INVALID_TYPE,
        CONSTRUCTION_FAILED
    };
    class LUX_EDITOR_CONFIGURATION_PUBLIC ConfigurationValue final
    {
    public:
        [[nodiscard]] static cxx::expected<ConfigurationValue, EConfigurationError> create(
            ConfigurationDescriptor,
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
        [[nodiscard]] const void* data() const noexcept
        {
            return value_.data();
        }
        [[nodiscard]] cxx::TypeToken type() const noexcept { return descriptor_.codec.type; }
        [[nodiscard]] serialization::SerializationResult encode(std::vector<std::byte>&) const noexcept;
        [[nodiscard]] serialization::SerializationResult decode(std::span<const std::byte>) noexcept;

    private:
        friend class settings::SettingsEntry;
        // Staged reflection can only be used for non-escaping validation inside SettingsEntry.
        [[nodiscard]] static cxx::expected<ConfigurationValue, EConfigurationError> create(
            ConfigurationDescriptor, std::shared_ptr<const void>, meta::ReflectionRegistry&
        ) noexcept;
        ConfigurationValue(std::shared_ptr<const void>, meta::RuntimeObject, ConfigurationDescriptor) noexcept;
        std::shared_ptr<const void> code_;
        std::shared_ptr<const void> reflection_;
        meta::RuntimeObject value_;
        ConfigurationDescriptor descriptor_;
    };
}
