#include <lux/engine/render/resource/StorageMeta.hpp>
#include <lux/engine/render/resource/FrameDataCall.hpp>
#include <bit>

namespace lux::render
{
    RenderResult<void>
    validateResourceRegistration(const ResourceRegistration& factory) noexcept
    {
        if (!factory.descriptor || !factory.storage())
        {
            return cxx::unexpected(RenderError{kStorageFactory});
        }
        if (!factory.storage()->create) return cxx::unexpected(RenderError{kStorageFactory});
        if (auto valid = validateStorage(*factory.descriptor); !valid)
        {
            return valid;
        }
        const auto& type = *factory.descriptor;
        const auto& call = factory.frame_call;
        const bool has_inputs = !type.frame_inputs.empty();
        const bool is_invalid_call = has_inputs != (call.invoke != nullptr) ||
            call.channels.size() != type.frame_inputs.size();
        if (is_invalid_call) return cxx::unexpected(RenderError{kStorageContract});
        if (has_inputs && (!call.declaration_type.isValid() || !call.method_type.isValid() ||
            call.storage_type != factory.storage()->implementation_type))
            return cxx::unexpected(RenderError{kStorageContract});
        for (std::size_t i = 0; i < call.channels.size(); ++i)
        {
            const auto& channel = call.channels[i];
            const bool is_wrong_type = channel.element != type.frame_inputs[i] ||
                channel.declaration != call.declaration_type || channel.resource.name() != type.type.name();
            const bool is_invalid_layout = channel.size == 0 || !std::has_single_bit(channel.alignment);
            if (is_wrong_type || is_invalid_layout) return cxx::unexpected(RenderError{kStorageContract});
        }
        return {};
    }

} // namespace lux::render
