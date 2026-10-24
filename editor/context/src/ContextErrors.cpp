#include <lux/engine/editor/ContextErrors.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>

namespace lux::editor
{
    cxx::expected<void, error::Error> registerContextErrors() noexcept
    {
        static constexpr error::ErrorDescriptor descriptors[]{
            Errors::EditorDuplicateServiceTypeDescriptor,
            Errors::EditorServiceUseOutsideProjectLifetimeDescriptor,
            Errors::EditorServiceTypeIsNotRegisteredDescriptor,
            Errors::EditorRecursiveServiceFactoryDescriptor,
            Errors::EditorInvalidUiFactoryDescriptor,
            Errors::EditorDuplicateUiTypeDescriptor,
            Errors::EditorUnknownUiTypeDescriptor,
            Errors::EditorProjectServicesRequireOwnerThreadDescriptor,
            Errors::EditorEmptyServiceFactoryDescriptor,
            Errors::EditorNullServiceDescriptor,
            Errors::SceneProfileInvalidDescriptor,
            Errors::SceneProfileDuplicateDescriptor,
            Errors::SceneProfileUnknownDescriptor
        };
        static const auto registered = error::ErrorRegistry::instance().registerTypes(descriptors);
        return registered;
    }
} // namespace lux::editor
