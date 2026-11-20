#include <lux/engine/render/extensions/resource/StorageMeta.hpp>
#include <lux/engine/render/extensions/FrameDataCall.hpp>
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
        const bool has_ops = factory.storage()->ops_type.isValid();
        const bool is_invalid_ops = has_ops != (factory.storage()->ops != nullptr);
        const bool invalid_factory =
            !factory.descriptor || !factory.storage()->create || is_invalid_ops;
        if (invalid_factory)
        {
            return cxx::unexpected(RenderError{kStorageFactory});
        }
        if (auto valid = validateStorage(*factory.descriptor); !valid)
        {
            return valid;
        }
        if (factory.commands.size() != factory.descriptor->commands.size())
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        for (std::size_t i = 0; i < factory.commands.size(); ++i)
        {
            const auto& command = factory.commands[i];
            const bool is_invalid_route =
                !command.valid() || !command.isResourceCommand() || command.handlerType().isValid();
            const bool is_wrong_storage = command.storageType() != factory.storage()->implementation_type;
            if (is_invalid_route || is_wrong_storage)
            {
                return cxx::unexpected(RenderError{kCommandContract});
            }
            if (auto valid = compatibleCommands(*factory.descriptor->commands[i], *command.descriptor()); !valid)
            {
                return valid;
            }
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
