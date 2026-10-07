#include <lux/engine/editor/FrameworkErrors.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>

namespace lux::editor
{
    FrameworkResult<void> registerFrameworkErrors() noexcept
    {
        static constexpr error::ErrorDescriptor descriptors[]{
            Errors::SceneProfileFrozenDescriptor,
            Errors::SceneProfileNotFrozenDescriptor,
            Errors::SceneProfileInvalidDescriptor,
            Errors::SceneProfileDuplicateDescriptor,
            Errors::SceneProfileUnknownDescriptor,
            Errors::ProjectManifestDescriptor,
            Errors::ProjectPluginsDescriptor,
            Errors::ProjectCancelledDescriptor,
            Errors::ProjectClosingDescriptor,
            Errors::ProjectNeedsPreparedPluginsDescriptor,

            Errors::EditorUiClearDescriptor,
            Errors::EditorUiClearBusyDescriptor,
            Errors::UiRenderConfigurationDescriptor,
            Errors::EditorWindowHasNoNativeOutputDescriptor,
            Errors::EditorNativeUiOutputIsNotImplementedOnThisPlatformDescriptor,
            Errors::EditorProjectChangeInsideAHostOperationDescriptor,
            Errors::EditorInvalidLayoutItemDescriptor,
            Errors::EditorDuplicateUiInstanceNameDescriptor,
            Errors::EditorUiFactoryReturnedAnAttachedOrNullPaneDescriptor,
            Errors::EditorCannotMountProjectWindowsDescriptor,
            Errors::EditorRecursiveHostFrameDescriptor,
            Errors::UiCaptureDescriptor,
            Errors::EditorInvalidWindowExtentDescriptor,
            Errors::EditorEditorRequiresObjectThreadDescriptor,
            Errors::EditorProjectNeedsANameAndAbsoluteRootDescriptor,
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
            Errors::SceneDescriptionDescriptor,
            Errors::UiConfigurationEncodeDescriptor,
            Errors::EditorGlfwInitializationFailedDescriptor,
            Errors::EditorNativeWindowCreationFailedDescriptor,
            Errors::EditorRootInitializationFailedDescriptor,
            Errors::EditorNativeInputDeliveryFailedDescriptor,
            Errors::ProcessExecution0Descriptor,
            Errors::ProcessExecution1Descriptor,
            Errors::ProcessExecution2Descriptor,
            Errors::ProcessExecution3Descriptor,
            Errors::ProcessExecution4Descriptor,
            Errors::ProcessExecution5Descriptor,
            Errors::ProcessExecution6Descriptor,
            Errors::ProcessExecution7Descriptor,
            Errors::ProcessExecution8Descriptor,
            Errors::ProcessExecution9Descriptor,
            Errors::ProcessExecution10Descriptor,
            Errors::ProcessExecutionUnknownDescriptor
        };
        static const auto registered = error::ErrorRegistry::instance().registerTypes(descriptors);
        return registered;
    }
} // namespace lux::editor
