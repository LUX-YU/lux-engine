#include <array>
#include <lux/engine/render/extensions/resource/ResourceDefinition.hpp>

namespace lux::render
{
    RenderResult<void> validateStorage(const ResourceDefinition& descriptor) noexcept
    {
        const bool invalid_identity = !descriptor.type.isValid();
        const auto capabilities = descriptor.required_capabilities | descriptor.optional_capabilities;
        const bool invalid_capabilities =
            (capabilities & ~7u) != 0 || (descriptor.required_capabilities & descriptor.optional_capabilities) != 0;
        const bool invalid_domain = descriptor.domain > EResourceDomain::VIEW;
        const bool invalid_descriptor = invalid_identity || invalid_capabilities || invalid_domain;
        if (invalid_descriptor)
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        for (std::size_t i = 0; i < descriptor.commands.size(); ++i)
        {
            const auto* command = descriptor.commands[i];
            if (!command)
            {
                return cxx::unexpected(RenderError{kCommandContract});
            }
            for (std::size_t previous = 0; previous < i; ++previous)
            {
                if (descriptor.commands[previous]->name == command->name)
                {
                    return cxx::unexpected(RenderError{kCommandContract});
                }
            }
        }
        return {};
    }

    RenderResult<void> compatibleStorage(const ResourceDefinition& expected, const ResourceDefinition& actual) noexcept
    {
        if (!expected.hasSameContract(actual))
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
