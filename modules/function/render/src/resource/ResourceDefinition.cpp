#include <array>
#include <lux/engine/render/resource/ResourceDefinition.hpp>

namespace lux::render
{
    RenderResult<void> validateStorage(const ResourceDefinition& descriptor) noexcept
    {
        const bool invalid_identity = !descriptor.type.isValid();
        constexpr auto known = GpuResourceFeatures{EGpuResourceFeature::SAMPLED_IMAGE_ARRAY} |
            EGpuResourceFeature::BLOCK_COMPRESSION_BC | EGpuResourceFeature::STORAGE_BUFFER_ARRAY;
        const bool unknown = !known.containsAll(descriptor.required_features | descriptor.optional_features);
        const bool overlapping = descriptor.required_features.containsAny(descriptor.optional_features);
        const bool invalid_capabilities = unknown || overlapping;
        const bool invalid_domain = descriptor.domain > EResourceDomain::JOB;
        const bool invalid_descriptor = invalid_identity || invalid_capabilities || invalid_domain ||
            descriptor.lifetime > EResourceContentLifetime::PER_RUN;
        if (invalid_descriptor)
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        for (std::size_t i = 0; i < descriptor.frame_inputs.size(); ++i)
        {
            const auto type = descriptor.frame_inputs[i];
            const auto prior = descriptor.frame_inputs.first(i);
            const bool is_invalid_input = !type.isValid() || std::ranges::find(prior, type) != prior.end();
            if (is_invalid_input) return cxx::unexpected(RenderError{kStorageContract});
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
            error::ErrorDescriptor{"lux.render.mesh.pipeline_not_ready", "Mesh pipeline is not prepared (material program slot, purpose, mesh slot)"},
            error::ErrorDescriptor{"lux.render.resource.identity", "Invalid storage identity"},
            error::ErrorDescriptor{"lux.render.resource.capacity", "Storage capacity exhausted"}
        };
        return descriptors;
    }
} // namespace lux::render
