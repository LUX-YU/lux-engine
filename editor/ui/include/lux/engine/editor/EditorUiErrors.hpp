#pragma once
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/error/ErrorDescriptor.hpp>

namespace lux::editor
{
    // Register this component during assembly, before invoking its operations.
    [[nodiscard]] cxx::expected<void, error::Error> registerEditorUiErrors() noexcept;

    namespace Errors
    {
        inline constexpr error::ErrorDescriptor UiRenderConfigurationDescriptor{
            "lux.ui.render_configuration",
            "UI render configuration code {0}",
            error::ERecovery::PERMANENT,
            {error::EArgument::UNSIGNED}
        };
        inline constexpr error::ErrorId UiRenderConfiguration = error::errorId(UiRenderConfigurationDescriptor.name);
        inline constexpr error::ErrorDescriptor UiCaptureDescriptor{
            "lux.ui.capture",
            "UI capture code {0}",
            error::ERecovery::PERMANENT,
            {error::EArgument::UNSIGNED}
        };
        inline constexpr error::ErrorId UiCapture = error::errorId(UiCaptureDescriptor.name);
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
    } // namespace Errors
} // namespace lux::editor
