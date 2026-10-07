#pragma once
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/editor/FrameworkResult.hpp>

namespace lux::editor
{
    class EditorContext;
    // Product composition belongs to the host lifetime, not to a single open request.
    using EditorAssembly = cxx::move_only_function<FrameworkResult<void>(EditorContext&) noexcept>;
} // namespace lux::editor
