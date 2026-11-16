#include <lux/engine/render/extensions/resource/StorageMeta.hpp>

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
        return compatibleStorage(type, *factory.descriptor);
    }

} // namespace lux::render
