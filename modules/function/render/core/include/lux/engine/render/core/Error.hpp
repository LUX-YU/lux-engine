#pragma once

#include <span>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/error/ErrorDescriptor.hpp>

namespace lux::render
{
    using RenderError = error::Error;

    template <typename T>
    using RenderResult = cxx::expected<T, RenderError>;

    inline constexpr auto kInvalidTargetExtent  = error::errorId("lux.render.core.invalid_target_extent");
    inline constexpr auto kInvalidTargetFormat  = error::errorId("lux.render.core.invalid_target_format");
    inline constexpr auto kInvalidTargetSamples = error::errorId("lux.render.core.invalid_target_samples");
    inline constexpr auto kInvalidTargetKind    = error::errorId("lux.render.core.invalid_target_kind");

    inline constexpr auto kInvalidViewport = error::errorId("lux.render.core.invalid_viewport");

    // Static descriptors for host registration. Core does not own a registry or log sink.
    [[nodiscard]] std::span<const error::ErrorDescriptor> renderCoreErrorDescriptors() noexcept;
}
