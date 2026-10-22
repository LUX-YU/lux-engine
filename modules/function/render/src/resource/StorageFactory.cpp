#include <lux/engine/render/resource/StorageFactory.hpp>

namespace lux::render
{
    RenderResult<void> validateStorageFactory(const StorageDescriptor& type, const StorageFactory& factory) noexcept
    {
        const bool has_config = factory.config_type.isValid();
        const bool is_invalid_config = has_config != (factory.compatible_config != nullptr);
        const bool has_ops = factory.ops_type.isValid();
        const bool is_invalid_ops = has_ops != (factory.ops != nullptr);
        const bool invalid_factory = !factory.descriptor || !factory.create || is_invalid_config || is_invalid_ops;
        if (invalid_factory)
        {
            return cxx::unexpected(RenderError{kStorageFactory});
        }
        if (auto valid = validateStorage(*factory.descriptor); !valid)
        {
            return valid;
        }
        return compatibleStorage(type, *factory.descriptor);
    }

} // namespace lux::render
