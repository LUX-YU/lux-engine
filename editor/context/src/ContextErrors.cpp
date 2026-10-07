#include <lux/engine/editor/ContextErrors.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>

namespace lux::editor
{
    cxx::expected<void, error::Error> registerContextErrors() noexcept
    {
        static constexpr error::ErrorDescriptor descriptors[]{
            Errors::EditorServiceRegistrationIsFrozenDescriptor,
            Errors::EditorDuplicateServiceTypeDescriptor,
            Errors::EditorServiceUseOutsideProjectLifetimeDescriptor,
            Errors::EditorServiceTypeIsNotRegisteredDescriptor,
            Errors::EditorRecursiveServiceFactoryDescriptor,
            Errors::EditorUiRegistrationIsFrozenDescriptor,
            Errors::EditorInvalidUiFactoryDescriptor,
            Errors::EditorDuplicateUiTypeDescriptor,
            Errors::EditorUiRegistrationIsNotFrozenDescriptor,
            Errors::EditorUnknownUiTypeDescriptor,
            Errors::EditorProjectServicesRequireOwnerThreadDescriptor,
            Errors::EditorEmptyServiceFactoryDescriptor,
            Errors::EditorNullServiceDescriptor,
            Errors::EditorSceneToolRegistrationIsFrozenDescriptor,
            Errors::EditorInvalidSceneToolFactoryDescriptor,
            Errors::EditorDuplicateSceneToolRuleDescriptor,
            Errors::EditorNullSceneToolSetDescriptor,
            Errors::EditorSceneToolRegistrationIsNotFrozenDescriptor,
            Errors::EditorAmbiguousSceneToolRulesDescriptor,
            Errors::EditorNoMatchingSceneToolRuleDescriptor,
            Errors::SceneProfileFrozenDescriptor,
            Errors::SceneProfileNotFrozenDescriptor,
            Errors::SceneProfileInvalidDescriptor,
            Errors::SceneProfileDuplicateDescriptor,
            Errors::SceneProfileUnknownDescriptor
        };
        static const auto registered = error::ErrorRegistry::instance().registerTypes(descriptors);
        return registered;
    }
} // namespace lux::editor
