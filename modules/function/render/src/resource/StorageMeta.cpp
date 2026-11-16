#include <lux/engine/render/extensions/resource/StorageMeta.hpp>
#include <lux/engine/render/extensions/FrameDataCall.hpp>
#include <bit>

namespace lux::render
{
    RenderResult<void>
    validateResourceRegistration(const ResourceDefinition& type, const ResourceRegistration& factory) noexcept
    {
        if (!factory.descriptor || !factory.storage)
        {
            return cxx::unexpected(RenderError{kStorageFactory});
        }
        const bool has_config = factory.storage->config_type.isValid();
        const bool is_invalid_config = has_config != (factory.storage->compatible_config != nullptr);
        const bool has_ops = factory.storage->ops_type.isValid();
        const bool is_invalid_ops = has_ops != (factory.storage->ops != nullptr);
        const bool invalid_factory =
            !factory.descriptor || !factory.storage->create || is_invalid_config || is_invalid_ops;
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
            const bool is_wrong_storage = command.storageType() != factory.storage->implementation_type;
            if (is_invalid_route || is_wrong_storage)
            {
                return cxx::unexpected(RenderError{kCommandContract});
            }
            if (auto valid = compatibleCommands(*factory.descriptor->commands[i], *command.descriptor()); !valid)
            {
                return valid;
            }
        }
        if (factory.frame_calls.size() != type.frame_inputs.size())
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        for (std::size_t i = 0; i < factory.frame_calls.size(); ++i)
        {
            const auto& call = factory.frame_calls[i];
            const bool is_invalid_route = call.target != EFrameDataTarget::STORAGE || call.handler_type.isValid() ||
                !call.resolve_storage || !call.invoke || call.root_size == 0 ||
                !std::has_single_bit(call.root_alignment) || !call.method_type.isValid();
            const bool is_wrong_type = call.data_type != type.frame_inputs[i] ||
                call.storage_type != factory.storage->implementation_type;
            if (is_invalid_route || is_wrong_type)
            {
                return cxx::unexpected(RenderError{kStorageContract});
            }
        }
        return compatibleStorage(type, *factory.descriptor);
    }

} // namespace lux::render
