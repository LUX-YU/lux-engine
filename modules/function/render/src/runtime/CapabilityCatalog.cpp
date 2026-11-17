#include <lux/engine/render/runtime/RenderServerImpl.hpp>
#include <ranges>
#include <set>

namespace lux::render
{
    RenderResult<const ConfigValue*> RenderServer::Impl::qualifyConfiguration(
        const VCapabilityConfiguration& input,
        const CapabilityImplementation& implementation,
        ConfigValue& decoded
    ) const noexcept
    {
        const auto* value = std::get_if<ConfigValue>(&input);
        const auto* serialized_input = std::get_if<SerializedRenderConfiguration>(&input);
        if (serialized_input)
        {
            const auto& contract = implementation.configuration;
            const bool is_wrong_schema =
                serialized_input->schema != contract.schema || serialized_input->version != contract.version;
            const bool exceeds_size = serialized_input->bytes.size() > config_.max_config_bytes;
            if (is_wrong_schema || exceeds_size)
            {
                return cxx::unexpected(RenderError{kInvalidConfig});
            }
        }
        const bool uses_typed_default = value && !value->type().isValid();
        const bool uses_saved_default = serialized_input && serialized_input->bytes.empty();
        if (uses_typed_default || uses_saved_default)
        {
            // Empty configuration means the implementation's concrete default, not encoded object bytes.
            if (!implementation.make_default_configuration)
            {
                return cxx::unexpected(RenderError{kInvalidConfig});
            }
            auto defaults = implementation.make_default_configuration();
            if (!defaults)
            {
                return cxx::unexpected(defaults.error());
            }
            decoded = std::move(*defaults);
            value = &decoded;
        }
        if (!value)
        {
            const auto& serialized = std::get<SerializedRenderConfiguration>(input);
            if (implementation.decode_configuration)
            {
                auto result = implementation.decode_configuration(serialized.bytes);
                if (!result)
                {
                    return cxx::unexpected(result.error());
                }
                decoded = std::move(*result);
            }
            else if (implementation.config_type.isValid() || !serialized.bytes.empty())
            {
                return cxx::unexpected(RenderError{kInvalidConfig});
            }
            value = &decoded;
        }
        const bool invalid_type = implementation.config_type != value->type();
        const bool invalid_size = value->retainedBytes() > config_.max_config_bytes;
        if (invalid_type || invalid_size)
        {
            return cxx::unexpected(RenderError{kInvalidConfig});
        }
        return value;
    }

    const CapabilityGraphDescription*
    RenderServer::Impl::capabilityGraph(CapabilityImplementationId implementation_id) const noexcept
    {
        const auto* implementation = capability_registry_.find(implementation_id);
        return implementation ? &implementation->graph : nullptr;
    }

    RenderError RenderServer::Impl::capabilityOutputError(
        const CapabilityGraphDescription& capability,
        const EffectOutputDesc& output
    ) noexcept
    {
        if (const auto valid = validateEffectOutput(output); !valid)
        {
            return valid.error();
        }
        const bool has_format =
            std::ranges::find(capability.target_formats, output.format) != capability.target_formats.end();
        const bool has_samples =
            std::ranges::find(capability.target_samples, output.samples) != capability.target_samples.end();
        const bool has_kind = std::ranges::find(capability.target_kinds, output.kind) != capability.target_kinds.end();
        const bool supported = has_format && has_samples && has_kind;
        return supported ? RenderError{} : RenderError{kRuntimeUnsupported};
    }

    void RenderServer::Impl::listCapabilities(
        const ListCapabilities::Request& input,
        ListCapabilities::Response& result
    ) noexcept
    {
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
            published.configuration = support.configuration;
            published.config_type = support.config_type;
            published.portable_configuration =
                !implementation.config_type.isValid() || implementation.decode_configuration;
            published.capability = capability;
            for (const auto& port : implementation.graph.ports)
            {
                if (port.direction == ECapabilityPortDirection::OUTPUT)
                {
                    published.outputs.push_back(port.key);
                }
            }
            for (const auto* declaration : capability.resources)
            {
                const auto* factory = resources_->factory(declaration->type);
                const bool is_unsupported = !factory ||
                    (factory->descriptor->required_capabilities & ~device_->resourceCapabilities()) != 0;
                if (is_unsupported)
                {
                    published.unavailable = RenderError{kStorageCapability};
                    break;
                }
            }
            if (input.output)
            {
                if (auto error = capabilityOutputError(implementation.graph, *input.output); error.type != 0)
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

    void RenderServer::Impl::validateCapabilityConfiguration(
        const ValidateCapabilityConfiguration::Request& input,
        ValidateCapabilityConfiguration::Response& result
    ) noexcept
    {
        const auto* capability = capabilityGraph(input.implementation_id);
        if (!capability)
        {
            result.error = RenderError{kRuntimeIdentity};
            return;
        }
        const auto& implementation = *capability_registry_.find(input.implementation_id);
        ConfigValue decoded;
        auto configuration = qualifyConfiguration(input.configuration, implementation, decoded);
        if (!configuration)
        {
            result.error = configuration.error();
            return;
        }
        auto dependencies = resolveCapabilityResources(
            *capability_registry_.get(capability_registry_.capabilityOf(input.implementation_id))
        );
        result.error = dependencies ? RenderError{} : dependencies.error();
    }

} // namespace lux::render
