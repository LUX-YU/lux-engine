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
