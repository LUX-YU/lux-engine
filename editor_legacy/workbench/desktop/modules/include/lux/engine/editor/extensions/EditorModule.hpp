#pragma once
#include <cstdint>
#include <string_view>

namespace lux::editor::extensions
{
    struct EditorExtensionExports;
    using GetEditorExtension = const EditorExtensionExports*() noexcept;
    // Product-generated strong references and DLL exports use the same validated table.
    // Descriptors are module constants; this view never owns mutable registration state.
    struct EditorModuleDescriptor final
    {
        std::string_view name;
        std::uint32_t version{1};
        GetEditorExtension* exports{};
    };
    using GetEditorModule = const EditorModuleDescriptor&() noexcept;
} // namespace lux::editor::extensions
