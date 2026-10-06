#pragma once
#include <lux/engine/editor/FrameworkResult.hpp>

namespace lux::editor
{
    // Call during component assembly, before factories or business operations are used.
    [[nodiscard]] FrameworkResult<void> registerFrameworkErrors() noexcept;
    namespace Errors
    {
        inline constexpr error::ErrorId UiRenderConfiguration = error::errorId("lux.ui.render_configuration");
        inline constexpr error::ErrorId EditorWindowHasNoNativeOutput =
            error::errorId("lux.editor.window_has_no_native_output");
        inline constexpr error::ErrorId EditorNativeUiOutputIsNotImplementedOnThisPlatform =
            error::errorId("lux.editor.native_ui_output_is_not_implemented_on_this_platform");
        inline constexpr error::ErrorId EditorProjectChangeInsideAHostOperation =
            error::errorId("lux.editor.project_change_inside_a_host_operation");
        inline constexpr error::ErrorId EditorInvalidLayoutItem = error::errorId("lux.editor.invalid_layout_item");
        inline constexpr error::ErrorId EditorDuplicateUiInstanceName =
            error::errorId("lux.editor.duplicate_ui_instance_name");
        inline constexpr error::ErrorId EditorUiFactoryReturnedAnAttachedOrNullPane =
            error::errorId("lux.editor.ui_factory_returned_an_attached_or_null_pane");
        inline constexpr error::ErrorId EditorCannotMountProjectWindows =
            error::errorId("lux.editor.cannot_mount_project_windows");
        inline constexpr error::ErrorId EditorRecursiveHostFrame = error::errorId("lux.editor.recursive_host_frame");
        inline constexpr error::ErrorId UiCapture = error::errorId("lux.ui.capture");
        inline constexpr error::ErrorId EditorInvalidWindowExtent = error::errorId("lux.editor.invalid_window_extent");
        inline constexpr error::ErrorId EditorEditorRequiresObjectThread =
            error::errorId("lux.editor.editor_requires_object_thread");
        inline constexpr error::ErrorId EditorProjectNeedsANameAndAbsoluteRoot =
            error::errorId("lux.editor.project_needs_a_name_and_absolute_root");
        inline constexpr error::ErrorId EditorServiceRegistrationIsFrozen =
            error::errorId("lux.editor.service_registration_is_frozen");
        inline constexpr error::ErrorId EditorDuplicateServiceType =
            error::errorId("lux.editor.duplicate_service_type");
        inline constexpr error::ErrorId EditorServiceUseOutsideProjectLifetime =
            error::errorId("lux.editor.service_use_outside_project_lifetime");
        inline constexpr error::ErrorId EditorServiceTypeIsNotRegistered =
            error::errorId("lux.editor.service_type_is_not_registered");
        inline constexpr error::ErrorId EditorRecursiveServiceFactory =
            error::errorId("lux.editor.recursive_service_factory");
        inline constexpr error::ErrorId EditorUiRegistrationIsFrozen =
            error::errorId("lux.editor.ui_registration_is_frozen");
        inline constexpr error::ErrorId EditorInvalidUiFactory = error::errorId("lux.editor.invalid_ui_factory");
        inline constexpr error::ErrorId EditorDuplicateUiType = error::errorId("lux.editor.duplicate_ui_type");
        inline constexpr error::ErrorId EditorUiRegistrationIsNotFrozen =
            error::errorId("lux.editor.ui_registration_is_not_frozen");
        inline constexpr error::ErrorId EditorUnknownUiType = error::errorId("lux.editor.unknown_ui_type");
        inline constexpr error::ErrorId EditorProjectServicesRequireOwnerThread =
            error::errorId("lux.editor.project_services_require_owner_thread");
        inline constexpr error::ErrorId EditorEmptyServiceFactory = error::errorId("lux.editor.empty_service_factory");
        inline constexpr error::ErrorId EditorNullService = error::errorId("lux.editor.null_service");
        inline constexpr error::ErrorId EditorSceneToolRegistrationIsFrozen =
            error::errorId("lux.editor.scene_tool_registration_is_frozen");
        inline constexpr error::ErrorId EditorInvalidSceneToolFactory =
            error::errorId("lux.editor.invalid_scene_tool_factory");
        inline constexpr error::ErrorId EditorDuplicateSceneToolRule =
            error::errorId("lux.editor.duplicate_scene_tool_rule");
        inline constexpr error::ErrorId EditorNullSceneToolSet = error::errorId("lux.editor.null_scene_tool_set");
        inline constexpr error::ErrorId EditorSceneToolRegistrationIsNotFrozen =
            error::errorId("lux.editor.scene_tool_registration_is_not_frozen");
        inline constexpr error::ErrorId EditorAmbiguousSceneToolRules =
            error::errorId("lux.editor.ambiguous_scene_tool_rules");
        inline constexpr error::ErrorId EditorNoMatchingSceneToolRule =
            error::errorId("lux.editor.no_matching_scene_tool_rule");
        inline constexpr error::ErrorId SceneDescription = error::errorId("lux.scene.description");
        inline constexpr error::ErrorId UiConfigurationEncode = error::errorId("lux.ui.configuration_encode");
        inline constexpr error::ErrorId EditorGlfwInitializationFailed =
            error::errorId("lux.editor.glfw_initialization_failed");
        inline constexpr error::ErrorId EditorNativeWindowCreationFailed =
            error::errorId("lux.editor.native_window_creation_failed");
        inline constexpr error::ErrorId EditorRootInitializationFailed =
            error::errorId("lux.editor.root_initialization_failed");
        inline constexpr error::ErrorId EditorNativeInputDeliveryFailed =
            error::errorId("lux.editor.native_input_delivery_failed");
    } // namespace Errors
} // namespace lux::editor
