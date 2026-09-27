#include <lux/engine/editor/metadata/ConfigurationValue.hpp>
#include <lux/engine/ui/Element.hpp>

namespace lux::editor
{
    ConfigurationValue::ConfigurationValue(
        std::shared_ptr<const void> code,
        meta::RuntimeObject value,
        ConfigurationEditorRegistration registration
    ) noexcept
        : code_(std::move(code)), value_(std::move(value)), registration_(registration)
    {}

    project::PluginResult<ConfigurationValue> ConfigurationValue::create(
        const ConfigurationEditorRegistration& registration,
        std::shared_ptr<const void> code
    ) noexcept
    {
        const bool invalid = !registration.reflection || !registration.codec.valid() || !registration.schema_name ||
                             !registration.schema_version || !meta::ReflectionRegistry::initialized();
        if (invalid)
            return lux::cxx::unexpected(
                project::PluginFailure{project::EPluginError::INVALID_EXPORT, {}, "configuration"}
            );
        const auto* reflection = registration.reflection(meta::ReflectionRegistry::instance());
        const bool invalid_type = !reflection || reflection->type.ptr != reflection ||
                                  reflection->type.hash != registration.codec.type.hash() ||
                                  reflection->type.name != registration.codec.type.name();
        if (invalid_type)
            return lux::cxx::unexpected(
                project::PluginFailure{project::EPluginError::INVALID_EXPORT, {}, registration.schema_name}
            );
        auto value = meta::RuntimeObject::create(reflection);
        if (!value)
            return lux::cxx::unexpected(
                project::PluginFailure{project::EPluginError::REGISTRATION_FAILURE, {}, registration.schema_name}
            );
        return ConfigurationValue(std::move(code), std::move(*value), registration);
    }

    ConfigurationValue& ConfigurationValue::operator=(ConfigurationValue&& other) noexcept
    {
        if (this != &other)
        {
            value_ = meta::RuntimeObject{};
            code_ = std::move(other.code_);
            value_ = std::move(other.value_);
            registration_ = other.registration_;
        }
        return *this;
    }

    EditorResult<std::unique_ptr<lux::ui::Element>> ConfigurationValue::createElement(
        lux::ui::Element& parent,
        lux::ui::ElementId id
    ) noexcept
    {
        if (!registration_.create)
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "configuration.editor"});
        return registration_.create(parent, std::move(id), *this);
    }

    serialization::SerializationResult ConfigurationValue::encode(std::vector<std::byte>& bytes) const noexcept
    {
        return registration_.codec.encode(value_.data(), bytes);
    }

    serialization::SerializationResult ConfigurationValue::decode(std::span<const std::byte> bytes) noexcept
    {
        // A rejected payload must not partially edit the live configuration value.
        auto candidate = create(registration_, code_);
        if (!candidate)
            return lux::cxx::unexpected(
                serialization::SerializationFailure{serialization::ESerializationError::INVALID_VALUE}
            );
        auto decoded = registration_.codec.decode(bytes, candidate->data());
        if (decoded)
            *this = std::move(*candidate);
        return decoded;
    }
} // namespace lux::editor
