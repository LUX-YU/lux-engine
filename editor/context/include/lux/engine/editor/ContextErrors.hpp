#pragma once
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/error/ErrorDescriptor.hpp>

namespace lux::editor
{
    // Register this component during assembly, before invoking its operations.
    [[nodiscard]] cxx::expected<void, error::Error> registerContextErrors() noexcept;

    namespace Errors
    {
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
        inline constexpr error::ErrorDescriptor SceneProfileInvalidDescriptor{
            "lux.editor.scene_profile.invalid",
            "Invalid scene profile declaration",
            error::ERecovery::PERMANENT,
            {}
        };
        inline constexpr error::ErrorId SceneProfileInvalid = error::errorId(SceneProfileInvalidDescriptor.name);
        inline constexpr error::ErrorDescriptor SceneProfileDuplicateDescriptor{
            "lux.editor.scene_profile.duplicate",
            "Duplicate scene profile identity",
            error::ERecovery::PERMANENT,
            {}
        };
        inline constexpr error::ErrorId SceneProfileDuplicate = error::errorId(SceneProfileDuplicateDescriptor.name);
        inline constexpr error::ErrorDescriptor SceneProfileUnknownDescriptor{
            "lux.editor.scene_profile.unknown",
            "Unknown scene profile identity",
            error::ERecovery::PERMANENT,
            {}
        };
        inline constexpr error::ErrorId SceneProfileUnknown = error::errorId(SceneProfileUnknownDescriptor.name);
        inline constexpr error::ErrorDescriptor ValueEditorInvalidDescriptor{
            "lux.editor.value_editor.invalid",
            "Invalid Editor value declaration",
            error::ERecovery::PERMANENT,
            {}
        };
        inline constexpr error::ErrorId ValueEditorInvalid = error::errorId(ValueEditorInvalidDescriptor.name);
        inline constexpr error::ErrorDescriptor ValueEditorDuplicateDescriptor{
            "lux.editor.value_editor.duplicate",
            "Duplicate Editor value type",
            error::ERecovery::PERMANENT,
            {}
        };
        inline constexpr error::ErrorId ValueEditorDuplicate = error::errorId(ValueEditorDuplicateDescriptor.name);
    } // namespace Errors
} // namespace lux::editor
