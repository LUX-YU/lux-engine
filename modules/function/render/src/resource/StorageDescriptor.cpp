#include <array>
#include <lux/engine/render/resource/StorageDescriptor.hpp>

namespace lux::render
{
    RenderResult<void> validateStorage(const StorageDescriptor& descriptor) noexcept
    {
        const bool invalid_identity = !descriptor.type.isValid();
        const auto capabilities = descriptor.required_capabilities | descriptor.optional_capabilities;
        const bool invalid_capabilities =
            (capabilities & ~3u) != 0 || (descriptor.required_capabilities & descriptor.optional_capabilities) != 0;
        if (invalid_identity || invalid_capabilities)
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        return {};
    }

    RenderResult<void> compatibleStorage(const StorageDescriptor& expected, const StorageDescriptor& actual) noexcept
    {
        const bool identity_mismatch = expected.type != actual.type;
        const bool policy_mismatch = expected.required_capabilities != actual.required_capabilities ||
                                     expected.optional_capabilities != actual.optional_capabilities;
        const bool binding_mismatch = expected.binding_type != actual.binding_type;
        if (identity_mismatch || policy_mismatch || binding_mismatch)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        return {};
    }

    std::span<const error::ErrorDescriptor> renderResourceErrorDescriptors() noexcept
    {
        static constexpr std::array descriptors{
            error::ErrorDescriptor{"lux.render.resource.cpu_stopped", "CPU preparation stopped"},
            error::ErrorDescriptor{"lux.render.resource.contract", "Invalid storage contract"},
            error::ErrorDescriptor{"lux.render.resource.mismatch", "Incompatible storage contract"},
            error::ErrorDescriptor{"lux.render.resource.factory", "Missing or conflicting storage factory"},
            error::ErrorDescriptor{"lux.render.resource.capability", "Storage capability unavailable"},
            error::ErrorDescriptor{"lux.render.resource.identity", "Invalid storage identity"},
            error::ErrorDescriptor{"lux.render.resource.capacity", "Storage capacity exhausted"}
        };
        return descriptors;
    }
} // namespace lux::render
