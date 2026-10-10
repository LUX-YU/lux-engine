#pragma once

#include <lux/engine/function/render/features/visibility.h>
#include <span>

namespace lux::render
{
    struct KernelDeclaration;
    /// Immutable declarations only. The accepting renderer registers and pins their module.
    [[nodiscard]] LUX_ENGINE_FUNCTION_RENDER_FEATURES_PUBLIC std::span<const KernelDeclaration>
    builtinKernelDeclarations() noexcept;
} // namespace lux::render
