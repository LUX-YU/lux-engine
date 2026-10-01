#pragma once
#include <lux/engine/editor/configuration/visibility.h>
#include <lux/engine/meta/RuntimeObject.hpp>
#include <lux/engine/serialization/PortableValueCodec.hpp>
namespace lux::editor
{
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
        [[nodiscard]] serialization::SerializationResult encode(std::vector<std::byte>&) const noexcept;
        [[nodiscard]] serialization::SerializationResult decode(std::span<const std::byte>) noexcept;

    private:
        ConfigurationValue(std::shared_ptr<const void>, meta::RuntimeObject, ConfigurationDescriptor) noexcept;
        std::shared_ptr<const void> code_;
        std::shared_ptr<const void> reflection_;
        meta::RuntimeObject value_;
        ConfigurationDescriptor descriptor_;
    };
}
