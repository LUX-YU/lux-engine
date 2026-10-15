#include <lux/engine/render/resource/Storage.hpp>
#include <lux/engine/render/resource/Factory.hpp>

#include <array>
#include <algorithm>

namespace lux::render
{
    RenderResult<void> validateStorageFactory(
        const StorageDescriptor& type, const StorageFactory& factory, EGraphicsApi api
    ) noexcept
    {
        const bool invalid_factory = !factory.description || !factory.ops || !factory.create ||
            !factory.compatible_config || factory.api != api;
        if (invalid_factory) return cxx::unexpected(RenderError{kStorageFactory});
        if (auto valid = validateStorage(*factory.description); !valid) return valid;
        return compatibleStorage(type, *factory.description);
    }

    RenderResult<void> validateStorage(const StorageDescriptor& descriptor) noexcept
    {
        const bool invalid_identity = !descriptor.type.isValid() || descriptor.abi.major == 0;
        const bool invalid_scopes = descriptor.scopes == 0 || (descriptor.scopes & ~15u) != 0;
        const auto capabilities = descriptor.required_capabilities | descriptor.optional_capabilities;
        const bool invalid_capabilities = (capabilities & ~3u) != 0 ||
            (descriptor.required_capabilities & descriptor.optional_capabilities) != 0;
        if (invalid_identity || invalid_scopes || invalid_capabilities)
            return cxx::unexpected(RenderError{kStorageContract});
        for (std::size_t i = 0; i < descriptor.bindings.size(); ++i)
        {
            const auto& binding = descriptor.bindings[i];
            const bool invalid_binding = binding.kind > EDescriptorKind::STORAGE_BUFFER ||
                (binding.kind == EDescriptorKind::STORAGE_BUFFER && binding.count != 1);
            if (invalid_binding) return cxx::unexpected(RenderError{kStorageContract});
            for (std::size_t j = 0; j < i; ++j)
                if (descriptor.bindings[j].set == binding.set && descriptor.bindings[j].binding == binding.binding)
                    return cxx::unexpected(RenderError{kStorageContract});
        }
        if (descriptor.config)
            if (auto valid = validateDataSchema(*descriptor.config); !valid) return valid;
        if (descriptor.binding_schema) return validateDataSchema(*descriptor.binding_schema);
        return {};
    }

    RenderResult<void> compatibleStorage(const StorageDescriptor& expected, const StorageDescriptor& actual) noexcept
    {
        const bool identity_mismatch = expected.type != actual.type || expected.abi != actual.abi;
        const bool policy_mismatch = expected.scopes != actual.scopes ||
            expected.required_capabilities != actual.required_capabilities ||
            expected.optional_capabilities != actual.optional_capabilities;
        const bool config_presence_mismatch = bool(expected.config) != bool(actual.config);
        const bool binding_mismatch = !std::ranges::equal(expected.bindings, actual.bindings) ||
            bool(expected.binding_schema) != bool(actual.binding_schema);
        if (identity_mismatch || policy_mismatch || config_presence_mismatch || binding_mismatch)
            return cxx::unexpected(RenderError{kStorageMismatch});
        if (expected.config)
            if (auto valid = compatibleDataSchemas(*expected.config, *actual.config); !valid) return valid;
        if (expected.binding_schema) return compatibleDataSchemas(*expected.binding_schema, *actual.binding_schema);
        return {};
    }

    std::span<const error::ErrorDescriptor> renderResourceErrorDescriptors() noexcept
    {
        static constexpr std::array descriptors{
            error::ErrorDescriptor{"lux.render.resource.contract", "Invalid storage contract"},
            error::ErrorDescriptor{"lux.render.resource.mismatch", "Incompatible storage contract"},
            error::ErrorDescriptor{"lux.render.resource.factory", "Missing or conflicting storage factory"},
            error::ErrorDescriptor{"lux.render.resource.capability", "Storage capability unavailable"},
            error::ErrorDescriptor{"lux.render.resource.domain", "Invalid storage domain"},
            error::ErrorDescriptor{"lux.render.resource.capacity", "Storage capacity exhausted"}
        };
        return descriptors;
    }
}
