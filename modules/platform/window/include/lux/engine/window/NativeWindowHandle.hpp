#pragma once

#include <variant>

namespace lux::window
{
    struct Win32WindowHandle final
    {
        void* window{};
    };

    /// Non-owning native presentation handle. The window must outlive all surface uses.
    /// monostate means this backend does not expose a supported presentation handle.
    using VNativeWindowHandle = std::variant<std::monostate, Win32WindowHandle>;
} // namespace lux::window
