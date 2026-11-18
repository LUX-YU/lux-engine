#include <lux/engine/render/runtime/RenderServerImpl.hpp>
#include <algorithm>

namespace lux::render
{
    RenderError RenderServer::Impl::capabilityOutputError(
        const CapabilityImplementation& capability,
        const EffectOutputDesc& output
    ) noexcept
    {
        if (const auto valid = validateEffectOutput({output.format, output.samples, output.kind}); !valid)
        {
            return valid.error();
        }
        const CapabilityOutputSupport requested{output.format, output.samples, output.kind};
        const bool supported = std::ranges::binary_search(capability.outputs, requested);
        return supported ? RenderError{} : RenderError{kRuntimeUnsupported};
    }

    void RenderServer::Impl::listCapabilities(
        const ListCapabilities::Request& input,
        ListCapabilities::Response& result
    ) noexcept
    {
        const auto* device = runtime_->device(input.device);
        if (!device)
        {
            result.error = {kRuntimeIdentity};
            return;
        }
        const auto capabilities = device->session().resourceCapabilities();
        for (const auto id : capability_registry_.implementations())
        {
            const auto& implementation = *capability_registry_.find(id);
            if (!input.implementation.empty() && input.implementation != implementation.implementation)
            {
                continue;
            }
            const auto& support = implementation;
            const auto capability_id = capability_registry_.capabilityOf(id);
            const auto& capability = *capability_registry_.get(capability_id);
            const bool selected = input.capability.empty() || capability.type.name() == input.capability;
            if (!selected)
            {
                continue;
            }
            auto& published = result.implementations.emplace_back();
            published.implementation_id = id;
            published.implementation = support.implementation;
            published.description = support.description;
            published.capability = capability;
            published.features = implementation.features;
            for (const auto type : capability_registry_.resources(capability_id))
            {
                const auto* factory = runtime_->resources_.factory(type);
                const bool is_unsupported =
                    !factory || !factory->descriptor->supports(capabilities);
                if (is_unsupported)
                {
                    published.unavailable = RenderError{kStorageCapability};
                    break;
                }
            }
            if (input.output)
            {
                if (auto error = capabilityOutputError(implementation, *input.output); error.type != 0)
                {
                    published.unavailable = error;
                }
            }
            for (auto dependency : implementation.features)
            {
                if (!features_.find(dependency))
                {
                    published.unavailable = RenderError{kFeatureMissingProvider};
                    break;
                }
            }
        }
        result.error = {};
    }

} // namespace lux::render
