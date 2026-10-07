#include <lux/engine/editor/EditorUiErrors.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>

namespace lux::editor
{
    cxx::expected<void, error::Error> registerEditorUiErrors() noexcept
    {
        static constexpr error::ErrorDescriptor descriptors[]{
            Errors::UiRenderConfigurationDescriptor,
            Errors::UiCaptureDescriptor,
            Errors::SceneDescriptionDescriptor,
            Errors::UiConfigurationEncodeDescriptor,
            Errors::EditorGlfwInitializationFailedDescriptor,
            Errors::EditorNativeWindowCreationFailedDescriptor,
            Errors::EditorRootInitializationFailedDescriptor,
            Errors::EditorNativeInputDeliveryFailedDescriptor
        };
        static const auto registered = error::ErrorRegistry::instance().registerTypes(descriptors);
        return registered;
    }
} // namespace lux::editor
