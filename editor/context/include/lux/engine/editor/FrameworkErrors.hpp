#pragma once
#include <lux/engine/editor/FrameworkResult.hpp>
#include <lux/engine/error/ErrorDescriptor.hpp>

namespace lux::editor
{
    // Call during component assembly, before factories or business operations are used.
    [[nodiscard]] FrameworkResult<void> registerFrameworkErrors() noexcept;
    namespace Errors
    {
        inline constexpr error::ErrorDescriptor EditorUiClearDescriptor{
            "lux.editor.ui_clear",
            "Cannot clear project windows: UI code {0}",
            error::ERecovery::PERMANENT,
            {error::EArgument::UNSIGNED}
        };
        inline constexpr error::ErrorId EditorUiClear = error::errorId(EditorUiClearDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorUiClearBusyDescriptor{
            "lux.editor.ui_clear_busy",
            "Cannot clear project windows: UI code {0}",
            error::ERecovery::RETRYABLE,
            {error::EArgument::UNSIGNED}
        };
        inline constexpr error::ErrorId EditorUiClearBusy = error::errorId(EditorUiClearBusyDescriptor.name);
        inline constexpr error::ErrorDescriptor UiRenderConfigurationDescriptor{
            "lux.ui.render_configuration",
            "UI render configuration code {0}",
            error::ERecovery::PERMANENT,
            {error::EArgument::UNSIGNED}
        };
        inline constexpr error::ErrorId UiRenderConfiguration = error::errorId(UiRenderConfigurationDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorWindowHasNoNativeOutputDescriptor{
            "lux.editor.window_has_no_native_output",
            "Window has no native output",
            error::ERecovery::PERMANENT
        };
        inline constexpr error::ErrorId EditorWindowHasNoNativeOutput =
            error::errorId(EditorWindowHasNoNativeOutputDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorNativeUiOutputIsNotImplementedOnThisPlatformDescriptor{
            "lux.editor.native_ui_output_is_not_implemented_on_this_platform",
            "Native UI output is not implemented on this platform",
            error::ERecovery::PERMANENT
        };
        inline constexpr error::ErrorId EditorNativeUiOutputIsNotImplementedOnThisPlatform =
            error::errorId(EditorNativeUiOutputIsNotImplementedOnThisPlatformDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorProjectChangeInsideAHostOperationDescriptor{
            "lux.editor.project_change_inside_a_host_operation",
            "Project change inside a host operation",
            error::ERecovery::RETRYABLE
        };
        inline constexpr error::ErrorId EditorProjectChangeInsideAHostOperation =
            error::errorId(EditorProjectChangeInsideAHostOperationDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorInvalidLayoutItemDescriptor{
            "lux.editor.invalid_layout_item",
            "Invalid layout item",
            error::ERecovery::PERMANENT
        };
        inline constexpr error::ErrorId EditorInvalidLayoutItem =
            error::errorId(EditorInvalidLayoutItemDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorDuplicateUiInstanceNameDescriptor{
            "lux.editor.duplicate_ui_instance_name",
            "Duplicate UI instance name",
            error::ERecovery::PERMANENT
        };
        inline constexpr error::ErrorId EditorDuplicateUiInstanceName =
            error::errorId(EditorDuplicateUiInstanceNameDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorUiFactoryReturnedAnAttachedOrNullPaneDescriptor{
            "lux.editor.ui_factory_returned_an_attached_or_null_pane",
            "UI factory returned an attached or null Pane",
            error::ERecovery::PERMANENT
        };
        inline constexpr error::ErrorId EditorUiFactoryReturnedAnAttachedOrNullPane =
            error::errorId(EditorUiFactoryReturnedAnAttachedOrNullPaneDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorCannotMountProjectWindowsDescriptor{
            "lux.editor.cannot_mount_project_windows",
            "Cannot mount project windows: code {0}",
            error::ERecovery::PERMANENT,
            {error::EArgument::UNSIGNED}
        };
        inline constexpr error::ErrorId EditorCannotMountProjectWindows =
            error::errorId(EditorCannotMountProjectWindowsDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorRecursiveHostFrameDescriptor{
            "lux.editor.recursive_host_frame",
            "Recursive host frame",
            error::ERecovery::RETRYABLE
        };
        inline constexpr error::ErrorId EditorRecursiveHostFrame =
            error::errorId(EditorRecursiveHostFrameDescriptor.name);
        inline constexpr error::ErrorDescriptor UiCaptureDescriptor{
            "lux.ui.capture",
            "UI capture code {0}",
            error::ERecovery::PERMANENT,
            {error::EArgument::UNSIGNED}
        };
        inline constexpr error::ErrorId UiCapture = error::errorId(UiCaptureDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorInvalidWindowExtentDescriptor{
            "lux.editor.invalid_window_extent",
            "Invalid window extent",
            error::ERecovery::PERMANENT
        };
        inline constexpr error::ErrorId EditorInvalidWindowExtent =
            error::errorId(EditorInvalidWindowExtentDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorEditorRequiresObjectThreadDescriptor{
            "lux.editor.editor_requires_object_thread",
            "Editor requires object thread",
            error::ERecovery::BUG
        };
        inline constexpr error::ErrorId EditorEditorRequiresObjectThread =
            error::errorId(EditorEditorRequiresObjectThreadDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorProjectNeedsANameAndAbsoluteRootDescriptor{
            "lux.editor.project_needs_a_name_and_absolute_root",
            "Project needs a name and absolute root",
            error::ERecovery::PERMANENT
        };
        inline constexpr error::ErrorId EditorProjectNeedsANameAndAbsoluteRoot =
            error::errorId(EditorProjectNeedsANameAndAbsoluteRootDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorServiceRegistrationIsFrozenDescriptor{
            "lux.editor.service_registration_is_frozen",
            "Service registration is frozen",
            error::ERecovery::PERMANENT
        };
        inline constexpr error::ErrorId EditorServiceRegistrationIsFrozen =
            error::errorId(EditorServiceRegistrationIsFrozenDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorDuplicateServiceTypeDescriptor{
            "lux.editor.duplicate_service_type",
            "Duplicate service type",
            error::ERecovery::PERMANENT
        };
        inline constexpr error::ErrorId EditorDuplicateServiceType =
            error::errorId(EditorDuplicateServiceTypeDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorServiceUseOutsideProjectLifetimeDescriptor{
            "lux.editor.service_use_outside_project_lifetime",
            "Service use outside project lifetime",
            error::ERecovery::PERMANENT
        };
        inline constexpr error::ErrorId EditorServiceUseOutsideProjectLifetime =
            error::errorId(EditorServiceUseOutsideProjectLifetimeDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorServiceTypeIsNotRegisteredDescriptor{
            "lux.editor.service_type_is_not_registered",
            "Service type is not registered",
            error::ERecovery::PERMANENT
        };
        inline constexpr error::ErrorId EditorServiceTypeIsNotRegistered =
            error::errorId(EditorServiceTypeIsNotRegisteredDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorRecursiveServiceFactoryDescriptor{
            "lux.editor.recursive_service_factory",
            "Recursive service factory",
            error::ERecovery::PERMANENT
        };
        inline constexpr error::ErrorId EditorRecursiveServiceFactory =
            error::errorId(EditorRecursiveServiceFactoryDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorUiRegistrationIsFrozenDescriptor{
            "lux.editor.ui_registration_is_frozen",
            "UI registration is frozen",
            error::ERecovery::PERMANENT
        };
        inline constexpr error::ErrorId EditorUiRegistrationIsFrozen =
            error::errorId(EditorUiRegistrationIsFrozenDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorInvalidUiFactoryDescriptor{
            "lux.editor.invalid_ui_factory",
            "Invalid UI factory",
            error::ERecovery::PERMANENT
        };
        inline constexpr error::ErrorId EditorInvalidUiFactory = error::errorId(EditorInvalidUiFactoryDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorDuplicateUiTypeDescriptor{
            "lux.editor.duplicate_ui_type",
            "Duplicate UI type",
            error::ERecovery::PERMANENT
        };
        inline constexpr error::ErrorId EditorDuplicateUiType = error::errorId(EditorDuplicateUiTypeDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorUiRegistrationIsNotFrozenDescriptor{
            "lux.editor.ui_registration_is_not_frozen",
            "UI registration is not frozen",
            error::ERecovery::PERMANENT
        };
        inline constexpr error::ErrorId EditorUiRegistrationIsNotFrozen =
            error::errorId(EditorUiRegistrationIsNotFrozenDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorUnknownUiTypeDescriptor{
            "lux.editor.unknown_ui_type",
            "Unknown UI type",
            error::ERecovery::PERMANENT
        };
        inline constexpr error::ErrorId EditorUnknownUiType = error::errorId(EditorUnknownUiTypeDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorProjectServicesRequireOwnerThreadDescriptor{
            "lux.editor.project_services_require_owner_thread",
            "Project services require owner thread",
            error::ERecovery::BUG
        };
        inline constexpr error::ErrorId EditorProjectServicesRequireOwnerThread =
            error::errorId(EditorProjectServicesRequireOwnerThreadDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorEmptyServiceFactoryDescriptor{
            "lux.editor.empty_service_factory",
            "Empty service factory",
            error::ERecovery::PERMANENT
        };
        inline constexpr error::ErrorId EditorEmptyServiceFactory =
            error::errorId(EditorEmptyServiceFactoryDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorNullServiceDescriptor{
            "lux.editor.null_service",
            "Null service",
            error::ERecovery::PERMANENT
        };
        inline constexpr error::ErrorId EditorNullService = error::errorId(EditorNullServiceDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorSceneToolRegistrationIsFrozenDescriptor{
            "lux.editor.scene_tool_registration_is_frozen",
            "Scene tool registration is frozen",
            error::ERecovery::PERMANENT
        };
        inline constexpr error::ErrorId EditorSceneToolRegistrationIsFrozen =
            error::errorId(EditorSceneToolRegistrationIsFrozenDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorInvalidSceneToolFactoryDescriptor{
            "lux.editor.invalid_scene_tool_factory",
            "Invalid scene tool factory",
            error::ERecovery::PERMANENT
        };
        inline constexpr error::ErrorId EditorInvalidSceneToolFactory =
            error::errorId(EditorInvalidSceneToolFactoryDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorDuplicateSceneToolRuleDescriptor{
            "lux.editor.duplicate_scene_tool_rule",
            "Duplicate scene tool rule",
            error::ERecovery::PERMANENT
        };
        inline constexpr error::ErrorId EditorDuplicateSceneToolRule =
            error::errorId(EditorDuplicateSceneToolRuleDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorNullSceneToolSetDescriptor{
            "lux.editor.null_scene_tool_set",
            "Null scene tool set",
            error::ERecovery::PERMANENT
        };
        inline constexpr error::ErrorId EditorNullSceneToolSet = error::errorId(EditorNullSceneToolSetDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorSceneToolRegistrationIsNotFrozenDescriptor{
            "lux.editor.scene_tool_registration_is_not_frozen",
            "Scene tool registration is not frozen",
            error::ERecovery::PERMANENT
        };
        inline constexpr error::ErrorId EditorSceneToolRegistrationIsNotFrozen =
            error::errorId(EditorSceneToolRegistrationIsNotFrozenDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorAmbiguousSceneToolRulesDescriptor{
            "lux.editor.ambiguous_scene_tool_rules",
            "Ambiguous scene tool rules",
            error::ERecovery::PERMANENT
        };
        inline constexpr error::ErrorId EditorAmbiguousSceneToolRules =
            error::errorId(EditorAmbiguousSceneToolRulesDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorNoMatchingSceneToolRuleDescriptor{
            "lux.editor.no_matching_scene_tool_rule",
            "No matching scene tool rule",
            error::ERecovery::PERMANENT
        };
        inline constexpr error::ErrorId EditorNoMatchingSceneToolRule =
            error::errorId(EditorNoMatchingSceneToolRuleDescriptor.name);
        inline constexpr error::ErrorDescriptor SceneDescriptionDescriptor{
            "lux.scene.description",
            "Scene description code {0}, system {1}, subject {2}",
            error::ERecovery::NEEDS_INPUT,
            {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::HEX}
        };
        inline constexpr error::ErrorId SceneDescription = error::errorId(SceneDescriptionDescriptor.name);
        inline constexpr error::ErrorDescriptor UiConfigurationEncodeDescriptor{
            "lux.ui.configuration_encode",
            "UI configuration encoding code {0}, offset {1}",
            error::ERecovery::NEEDS_INPUT,
            {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}
        };
        inline constexpr error::ErrorId UiConfigurationEncode = error::errorId(UiConfigurationEncodeDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorGlfwInitializationFailedDescriptor{
            "lux.editor.glfw_initialization_failed",
            "GLFW initialization failed",
            error::ERecovery::PERMANENT
        };
        inline constexpr error::ErrorId EditorGlfwInitializationFailed =
            error::errorId(EditorGlfwInitializationFailedDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorNativeWindowCreationFailedDescriptor{
            "lux.editor.native_window_creation_failed",
            "Native window creation failed: code {0}",
            error::ERecovery::PERMANENT,
            {error::EArgument::UNSIGNED}
        };
        inline constexpr error::ErrorId EditorNativeWindowCreationFailed =
            error::errorId(EditorNativeWindowCreationFailedDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorRootInitializationFailedDescriptor{
            "lux.editor.root_initialization_failed",
            "Root initialization failed: code {0}",
            error::ERecovery::PERMANENT,
            {error::EArgument::UNSIGNED}
        };
        inline constexpr error::ErrorId EditorRootInitializationFailed =
            error::errorId(EditorRootInitializationFailedDescriptor.name);
        inline constexpr error::ErrorDescriptor EditorNativeInputDeliveryFailedDescriptor{
            "lux.editor.native_input_delivery_failed",
            "Native input delivery failed: code {0}",
            error::ERecovery::PERMANENT,
            {error::EArgument::UNSIGNED}
        };
        inline constexpr error::ErrorId EditorNativeInputDeliveryFailed =
            error::errorId(EditorNativeInputDeliveryFailedDescriptor.name);
        inline constexpr error::ErrorDescriptor ProcessExecution0Descriptor{
            "lux.process.execution.0",
            "ExecutionRuntime code {0}",
            error::ERecovery::PERMANENT,
            {error::EArgument::UNSIGNED}
        };
        inline constexpr error::ErrorId ProcessExecution0 = error::errorId(ProcessExecution0Descriptor.name);
        inline constexpr error::ErrorDescriptor ProcessExecution1Descriptor{
            "lux.process.execution.1",
            "ExecutionRuntime code {0}",
            error::ERecovery::PERMANENT,
            {error::EArgument::UNSIGNED}
        };
        inline constexpr error::ErrorId ProcessExecution1 = error::errorId(ProcessExecution1Descriptor.name);
        inline constexpr error::ErrorDescriptor ProcessExecution2Descriptor{
            "lux.process.execution.2",
            "ExecutionRuntime code {0}",
            error::ERecovery::RETRYABLE,
            {error::EArgument::UNSIGNED}
        };
        inline constexpr error::ErrorId ProcessExecution2 = error::errorId(ProcessExecution2Descriptor.name);
        inline constexpr error::ErrorDescriptor ProcessExecution3Descriptor{
            "lux.process.execution.3",
            "ExecutionRuntime code {0}",
            error::ERecovery::PERMANENT,
            {error::EArgument::UNSIGNED}
        };
        inline constexpr error::ErrorId ProcessExecution3 = error::errorId(ProcessExecution3Descriptor.name);
        inline constexpr error::ErrorDescriptor ProcessExecution4Descriptor{
            "lux.process.execution.4",
            "ExecutionRuntime code {0}",
            error::ERecovery::PERMANENT,
            {error::EArgument::UNSIGNED}
        };
        inline constexpr error::ErrorId ProcessExecution4 = error::errorId(ProcessExecution4Descriptor.name);
        inline constexpr error::ErrorDescriptor ProcessExecution5Descriptor{
            "lux.process.execution.5",
            "ExecutionRuntime code {0}",
            error::ERecovery::PERMANENT,
            {error::EArgument::UNSIGNED}
        };
        inline constexpr error::ErrorId ProcessExecution5 = error::errorId(ProcessExecution5Descriptor.name);
        inline constexpr error::ErrorDescriptor ProcessExecution6Descriptor{
            "lux.process.execution.6",
            "ExecutionRuntime code {0}",
            error::ERecovery::PERMANENT,
            {error::EArgument::UNSIGNED}
        };
        inline constexpr error::ErrorId ProcessExecution6 = error::errorId(ProcessExecution6Descriptor.name);
        inline constexpr error::ErrorDescriptor ProcessExecution7Descriptor{
            "lux.process.execution.7",
            "ExecutionRuntime code {0}",
            error::ERecovery::PERMANENT,
            {error::EArgument::UNSIGNED}
        };
        inline constexpr error::ErrorId ProcessExecution7 = error::errorId(ProcessExecution7Descriptor.name);
        inline constexpr error::ErrorDescriptor ProcessExecution8Descriptor{
            "lux.process.execution.8",
            "ExecutionRuntime code {0}",
            error::ERecovery::RETRYABLE,
            {error::EArgument::UNSIGNED}
        };
        inline constexpr error::ErrorId ProcessExecution8 = error::errorId(ProcessExecution8Descriptor.name);
        inline constexpr error::ErrorDescriptor ProcessExecution9Descriptor{
            "lux.process.execution.9",
            "ExecutionRuntime code {0}",
            error::ERecovery::PERMANENT,
            {error::EArgument::UNSIGNED}
        };
        inline constexpr error::ErrorId ProcessExecution9 = error::errorId(ProcessExecution9Descriptor.name);
        inline constexpr error::ErrorDescriptor ProcessExecution10Descriptor{
            "lux.process.execution.10",
            "ExecutionRuntime code {0}",
            error::ERecovery::PERMANENT,
            {error::EArgument::UNSIGNED}
        };
        inline constexpr error::ErrorId ProcessExecution10 = error::errorId(ProcessExecution10Descriptor.name);
        inline constexpr error::ErrorDescriptor ProcessExecutionUnknownDescriptor{
            "lux.process.execution.unknown",
            "ExecutionRuntime code {0}",
            error::ERecovery::BUG,
            {error::EArgument::UNSIGNED}
        };
        inline constexpr error::ErrorId ProcessExecutionUnknown =
            error::errorId(ProcessExecutionUnknownDescriptor.name);
    } // namespace Errors
} // namespace lux::editor
