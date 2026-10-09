#include <lux/engine/editor/AppErrors.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>

namespace lux::editor
{
    cxx::expected<void, error::Error> registerAppErrors() noexcept
    {
        static constexpr error::ErrorDescriptor descriptors[]{
            Errors::EditorWindowHasNoNativeOutputDescriptor,
            Errors::EditorNativeUiOutputIsNotImplementedOnThisPlatformDescriptor,
            Errors::EditorRecursiveHostFrameDescriptor,
            Errors::EditorInvalidWindowExtentDescriptor,
            Errors::EditorEditorRequiresObjectThreadDescriptor
        };
        static const auto registered = error::ErrorRegistry::instance().registerTypes(descriptors);
        return registered;
    }
} // namespace lux::editor
