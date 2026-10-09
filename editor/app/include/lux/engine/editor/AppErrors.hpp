#pragma once
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/error/ErrorDescriptor.hpp>

namespace lux::editor
{
    // Register this component during assembly, before invoking its operations.
    [[nodiscard]] cxx::expected<void, error::Error> registerAppErrors() noexcept;

    namespace Errors
    {
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
        inline constexpr error::ErrorDescriptor EditorRecursiveHostFrameDescriptor{
            "lux.editor.recursive_host_frame",
            "Recursive host frame",
            error::ERecovery::RETRYABLE
        };
        inline constexpr error::ErrorId EditorRecursiveHostFrame =
            error::errorId(EditorRecursiveHostFrameDescriptor.name);
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
    } // namespace Errors
} // namespace lux::editor
