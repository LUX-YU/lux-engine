#include <lux/engine/render/resource/StorageMeta.hpp>
#include <bit>

namespace lux::render
{
    RenderResult<void>
    validateResourceRegistration(const ResourceRegistration& factory) noexcept
    {
        if (!factory.descriptor || !factory.storage().create)
        {
            return cxx::unexpected(RenderError{kStorageFactory});
        }
        if (auto valid = validateStorage(*factory.descriptor); !valid)
        {
            return valid;
        }
        const auto& type = *factory.descriptor;
        const auto* ops = factory.storage().bindings();
        if (ops && ops->settleFrame && !ops->prepareFrame) return cxx::unexpected(RenderError{kStorageContract});
        if (ops && ops->cpu_view)
        {
            const auto& view = *ops->cpu_view;
            if (!view.type.isValid() || !view.select) return cxx::unexpected(RenderError{kStorageContract});
        }
        const bool per_run = type.lifetime == EResourceContentLifetime::PER_RUN;
        if ((per_run && (!ops || !ops->selectRun)) || (!per_run && ops && ops->hasRequiredInput))
            return cxx::unexpected(RenderError{kStorageContract});
        const auto channels = factory.channels();
        const bool has_inputs = !type.frame_inputs.empty();
        const bool is_invalid_call = has_inputs != (factory.applyFrame() != nullptr) ||
            channels.size() != type.frame_inputs.size();
        if (is_invalid_call) return cxx::unexpected(RenderError{kStorageContract});
        for (std::size_t i = 0; i < channels.size(); ++i)
        {
            const auto& channel = channels[i];
            const auto& element = type.frame_inputs[i];
            const auto& declaration = channels.front().declaration;
            const bool wrong_element = channel.element != element || channel.element.name() != element.name();
            const bool wrong_declaration = !channel.declaration.isValid() || channel.declaration != declaration ||
                channel.declaration.name() != declaration.name() || channel.resource.name() != type.type.name();
            const bool is_invalid_layout = channel.size == 0 || !std::has_single_bit(channel.alignment);
            if (wrong_element || wrong_declaration || is_invalid_layout) return cxx::unexpected(RenderError{kStorageContract});
        }
        return {};
    }

} // namespace lux::render
