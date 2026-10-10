#pragma once

#include <span>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/error/ErrorDescriptor.hpp>

namespace lux::render
{
    using RenderError = error::Error;

    template <typename T>
    using RenderResult = cxx::expected<T, RenderError>;

    inline constexpr auto kInvalidIdentity = error::errorId("lux.render.core.invalid_identity");
    inline constexpr auto kInvalidLayout = error::errorId("lux.render.core.invalid_layout");
    inline constexpr auto kInvalidVersion = error::errorId("lux.render.core.invalid_version");
    inline constexpr auto kInvalidCapability = error::errorId("lux.render.core.invalid_capability");
    inline constexpr auto kIdentityCollision = error::errorId("lux.render.core.identity_collision");
    inline constexpr auto kDefinitionMismatch = error::errorId("lux.render.core.definition_mismatch");

    // Static storage. Hosts may register this span once with the common error
    // registry during assembly. Core neither owns nor invokes that service.
    [[nodiscard]] std::span<const error::ErrorDescriptor> renderCoreErrorDescriptors() noexcept;
}
