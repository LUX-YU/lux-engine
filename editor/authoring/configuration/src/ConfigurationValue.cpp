#include <lux/engine/editor/configuration/ConfigurationValue.hpp>
#include <lux/engine/editor/configuration/EditorReflection.hpp>

namespace lux::editor
{
    ConfigurationValue::ConfigurationValue(
        std::shared_ptr<const void> code,
        meta::RuntimeObject value,
        ConfigurationDescriptor registration
    ) noexcept
        : code_(std::move(code)), reflection_(acquireEditorReflection()), value_(std::move(value)),
          descriptor_(std::move(registration))
    {}

    cxx::expected<ConfigurationValue, EConfigurationError> ConfigurationValue::create(
        ConfigurationDescriptor registration,
        std::shared_ptr<const void> code
    ) noexcept
    {
        const bool invalid = !registration.reflection || !registration.codec.valid() ||
                             registration.schema_name.empty() || !registration.schema_version ||
                             !meta::ReflectionRegistry::initialized();
        if (invalid)
            return lux::cxx::unexpected(EConfigurationError::INVALID_DESCRIPTOR);
        const auto* reflection = registration.reflection(meta::ReflectionRegistry::instance());
        const bool invalid_type = !reflection || reflection->type.ptr != reflection ||
                                  reflection->type.hash != registration.codec.type.hash() ||
                                  reflection->type.name != registration.codec.type.name();
        if (invalid_type)
            return lux::cxx::unexpected(EConfigurationError::INVALID_TYPE);
        auto value = meta::RuntimeObject::create(reflection);
        if (!value)
            return lux::cxx::unexpected(EConfigurationError::CONSTRUCTION_FAILED);
        return ConfigurationValue(std::move(code), std::move(*value), std::move(registration));
    }

    ConfigurationValue& ConfigurationValue::operator=(ConfigurationValue&& other) noexcept
    {
        if (this != &other)
        {
            value_ = meta::RuntimeObject{};
            code_ = std::move(other.code_);
            reflection_ = std::move(other.reflection_);
            value_ = std::move(other.value_);
            descriptor_ = std::move(other.descriptor_);
        }
        return *this;
    }

    serialization::SerializationResult ConfigurationValue::encode(std::vector<std::byte>& bytes) const noexcept
    {
        return descriptor_.codec.encode(value_.data(), bytes);
    }

    serialization::SerializationResult ConfigurationValue::decode(std::span<const std::byte> bytes) noexcept
    {
        // A rejected payload must not partially edit the live configuration value.
        auto candidate = create(descriptor_, code_);
        if (!candidate)
            return lux::cxx::unexpected(
                serialization::SerializationFailure{serialization::ESerializationError::INVALID_VALUE}
            );
        auto decoded = descriptor_.codec.decode(bytes, candidate->data());
        if (decoded)
            *this = std::move(*candidate);
        return decoded;
    }
} // namespace lux::editor
