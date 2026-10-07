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
            Errors::EditorEditorRequiresObjectThreadDescriptor,
            Errors::ProcessExecution0Descriptor,
            Errors::ProcessExecution1Descriptor,
            Errors::ProcessExecution2Descriptor,
            Errors::ProcessExecution3Descriptor,
            Errors::ProcessExecution4Descriptor,
            Errors::ProcessExecution5Descriptor,
            Errors::ProcessExecution6Descriptor,
            Errors::ProcessExecution7Descriptor,
            Errors::ProcessExecution8Descriptor,
            Errors::ProcessExecution9Descriptor,
            Errors::ProcessExecution10Descriptor,
            Errors::ProcessExecutionUnknownDescriptor
        };
        static const auto registered = error::ErrorRegistry::instance().registerTypes(descriptors);
        return registered;
    }
} // namespace lux::editor
