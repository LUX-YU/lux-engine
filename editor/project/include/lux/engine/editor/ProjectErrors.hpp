#pragma once
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/error/ErrorDescriptor.hpp>

namespace lux::editor
{
    // Register this component during assembly, before invoking its operations.
    [[nodiscard]] cxx::expected<void, error::Error> registerProjectErrors() noexcept;

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
        inline constexpr error::ErrorDescriptor EditorProjectNeedsANameAndAbsoluteRootDescriptor{
            "lux.editor.project_needs_a_name_and_absolute_root",
            "Project needs a name and absolute root",
            error::ERecovery::PERMANENT
        };
        inline constexpr error::ErrorId EditorProjectNeedsANameAndAbsoluteRoot =
            error::errorId(EditorProjectNeedsANameAndAbsoluteRootDescriptor.name);
        inline constexpr error::ErrorDescriptor ProjectManifestDescriptor{
            "lux.editor.project.manifest",
            "Project manifest code {0}, record {1}, system {2}",
            error::ERecovery::PERMANENT,
            {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}
        };
        inline constexpr error::ErrorId ProjectManifest = error::errorId(ProjectManifestDescriptor.name);
        inline constexpr error::ErrorDescriptor ProjectPluginsDescriptor{
            "lux.editor.project.plugins",
            "Project plugin code {0}",
            error::ERecovery::PERMANENT,
            {error::EArgument::UNSIGNED}
        };
        inline constexpr error::ErrorId ProjectPlugins = error::errorId(ProjectPluginsDescriptor.name);
        inline constexpr error::ErrorDescriptor ProjectCancelledDescriptor{
            "lux.editor.project.cancelled",
            "Project preparation was cancelled",
            error::ERecovery::PERMANENT,
            {}
        };
        inline constexpr error::ErrorId ProjectCancelled = error::errorId(ProjectCancelledDescriptor.name);
        inline constexpr error::ErrorDescriptor ProjectClosingDescriptor{
            "lux.editor.project.closing",
            "Project is closing",
            error::ERecovery::PERMANENT,
            {}
        };
        inline constexpr error::ErrorId ProjectClosing = error::errorId(ProjectClosingDescriptor.name);
        inline constexpr error::ErrorDescriptor ProjectNeedsPreparedPluginsDescriptor{
            "lux.editor.project.needs_prepared_plugins",
            "Plugin projects require verified preparation by the host",
            error::ERecovery::PERMANENT,
            {}
        };
        inline constexpr error::ErrorId ProjectNeedsPreparedPlugins =
            error::errorId(ProjectNeedsPreparedPluginsDescriptor.name);
    } // namespace Errors
} // namespace lux::editor
