#include <lux/engine/editor/ProjectErrors.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>

namespace lux::editor
{
    cxx::expected<void, error::Error> registerProjectErrors() noexcept
    {
        static constexpr error::ErrorDescriptor descriptors[]{
            Errors::EditorUiClearDescriptor,
            Errors::EditorUiClearBusyDescriptor,
            Errors::EditorProjectChangeInsideAHostOperationDescriptor,
            Errors::EditorInvalidLayoutItemDescriptor,
            Errors::EditorDuplicateUiInstanceNameDescriptor,
            Errors::EditorUiFactoryReturnedAnAttachedOrNullPaneDescriptor,
            Errors::EditorCannotMountProjectWindowsDescriptor,
            Errors::EditorProjectNeedsANameAndAbsoluteRootDescriptor,
            Errors::ProjectManifestDescriptor,
            Errors::ProjectPluginsDescriptor,
            Errors::ProjectCancelledDescriptor,
            Errors::ProjectClosingDescriptor,
            Errors::ProjectNeedsPreparedPluginsDescriptor
        };
        static const auto registered = error::ErrorRegistry::instance().registerTypes(descriptors);
        return registered;
    }
} // namespace lux::editor
