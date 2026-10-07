#pragma once
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/editor/FrameworkResult.hpp>

namespace lux::editor
{
    class EditorComposition;
    // Product composition belongs to the host lifetime, not to a single open request.
    using EditorAssembly = cxx::move_only_function<FrameworkResult<void>(EditorComposition&) noexcept>;
} // namespace lux::editor
