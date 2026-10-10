#include <lux/engine/render/core/Descriptors.hpp>

#include <array>
#include <bit>

namespace lux::render
{
    namespace
    {
        constexpr std::array kErrorDescriptors{
            error::ErrorDescriptor{
                "lux.render.core.invalid_identity", "Invalid canonical identity (id, expected)",
                error::ERecovery::PERMANENT, {error::EArgument::HEX, error::EArgument::HEX}
            },
            error::ErrorDescriptor{
                "lux.render.core.invalid_layout", "Invalid value layout (id, size, alignment)",
                error::ERecovery::PERMANENT,
                {error::EArgument::HEX, error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}
            },
            error::ErrorDescriptor{
                "lux.render.core.invalid_version", "Zero contract version (id, wire/contract, layout)",
                error::ERecovery::PERMANENT,
                {error::EArgument::HEX, error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}
            },
            error::ErrorDescriptor{
                "lux.render.core.invalid_capability", "Invalid or duplicate capability (feature, capability, index)",
                error::ERecovery::PERMANENT,
                {error::EArgument::HEX, error::EArgument::HEX, error::EArgument::UNSIGNED}
            },
            error::ErrorDescriptor{
                "lux.render.core.identity_collision", "Different names share a semantic id (id)",
                error::ERecovery::PERMANENT, {error::EArgument::HEX}
            },
            error::ErrorDescriptor{
                "lux.render.core.definition_mismatch", "Incompatible data definitions (existing id, candidate id)",
                error::ERecovery::PERMANENT, {error::EArgument::HEX, error::EArgument::HEX}
            }
        };

        RenderResult<void> validateCapabilities(FeatureTypeId feature, std::span<const SceneCapabilityId> ids) noexcept
        {
            for (std::size_t index = 0; index < ids.size(); ++index)
            {
                if (!ids[index].isValid())
                {
                    return cxx::unexpected(RenderError{kInvalidCapability, {feature.value(), 0, index}});
                }
                for (std::size_t previous = 0; previous < index; ++previous)
                {
                    if (ids[previous] == ids[index])
                    {
                        return cxx::unexpected(
                            RenderError{kInvalidCapability, {feature.value(), ids[index].value(), index}}
                        );
                    }
                }
            }
            return {};
        }
    }

    std::span<const error::ErrorDescriptor> renderCoreErrorDescriptors() noexcept
    {
        return kErrorDescriptors;
    }

    RenderResult<void> validateDescriptor(const RenderDataDescriptor& descriptor) noexcept
    {
        const auto expected_id = renderDataTypeId(descriptor.canonical_name);
        const bool is_invalid_identity = !descriptor.id.isValid() || descriptor.id != expected_id;
        if (is_invalid_identity)
        {
            return cxx::unexpected(RenderError{kInvalidIdentity, {descriptor.id.value(), expected_id.value()}});
        }
        const bool is_invalid_version = descriptor.wire_version == 0 || descriptor.layout_version == 0;
        if (is_invalid_version)
        {
            return cxx::unexpected(RenderError{
                kInvalidVersion, {descriptor.id.value(), descriptor.wire_version, descriptor.layout_version}
            });
        }
        const bool has_valid_alignment = std::has_single_bit(descriptor.alignment);
        const bool is_invalid_layout = descriptor.size == 0 || !has_valid_alignment ||
            descriptor.size % descriptor.alignment != 0;
        if (is_invalid_layout)
        {
            return cxx::unexpected(RenderError{
                kInvalidLayout, {descriptor.id.value(), descriptor.size, descriptor.alignment}
            });
        }
        return {};
    }

    RenderResult<void> validateDescriptor(const SceneCapabilityDescriptor& descriptor) noexcept
    {
        const auto expected_id = sceneCapabilityId(descriptor.canonical_name);
        const bool is_invalid_identity = !descriptor.id.isValid() || descriptor.id != expected_id;
        if (is_invalid_identity)
        {
            return cxx::unexpected(RenderError{kInvalidIdentity, {descriptor.id.value(), expected_id.value()}});
        }
        if (descriptor.contract_version == 0)
        {
            return cxx::unexpected(RenderError{kInvalidVersion, {descriptor.id.value()}});
        }
        return {};
    }

    RenderResult<void> validateDescriptor(const FeatureDescriptor& descriptor) noexcept
    {
        const auto expected_id = featureTypeId(descriptor.canonical_name);
        const bool is_invalid_identity = !descriptor.id.isValid() || descriptor.id != expected_id;
        if (is_invalid_identity)
        {
            return cxx::unexpected(RenderError{kInvalidIdentity, {descriptor.id.value(), expected_id.value()}});
        }
        if (descriptor.contract_version == 0)
        {
            return cxx::unexpected(RenderError{kInvalidVersion, {descriptor.id.value()}});
        }
        auto provided = validateCapabilities(descriptor.id, descriptor.provides);
        if (!provided)
        {
            return provided;
        }
        return validateCapabilities(descriptor.id, descriptor.required_capabilities);
    }

    RenderResult<void>
    validateCompatible(const RenderDataDescriptor& existing, const RenderDataDescriptor& candidate) noexcept
    {
        const bool is_name_mismatch = existing.canonical_name != candidate.canonical_name;
        const bool is_id_match = existing.id == candidate.id;
        if (is_id_match && is_name_mismatch)
        {
            return cxx::unexpected(RenderError{kIdentityCollision, {candidate.id.value()}});
        }
        const bool is_definition_mismatch = !is_id_match || is_name_mismatch ||
            existing.wire_version != candidate.wire_version || existing.layout_version != candidate.layout_version ||
            existing.size != candidate.size || existing.alignment != candidate.alignment;
        if (is_definition_mismatch)
        {
            return cxx::unexpected(
                RenderError{kDefinitionMismatch, {existing.id.value(), candidate.id.value()}}
            );
        }
        return {};
    }
}
