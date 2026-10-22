#pragma once

#include <lux/engine/render/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/error/ErrorDescriptor.hpp>
#include <span>

namespace lux::render
{
    using RenderError = error::Error;

    template <typename T> using RenderResult = cxx::expected<T, RenderError>;

    inline constexpr auto kDeviceFailure = error::errorId("lux.render.device.native_failure");
    inline constexpr auto kDeviceDiagnostic = error::errorId("lux.render.device.diagnostic");
    inline constexpr auto kDeviceArgument = error::errorId("lux.render.device.invalid_argument");
    inline constexpr auto kDeviceUnsupported = error::errorId("lux.render.device.unsupported");
    inline constexpr auto kDeviceCapacity = error::errorId("lux.render.device.capacity");
    inline constexpr auto kDeviceBusy = error::errorId("lux.render.device.busy");
    inline constexpr auto kUploadStopped = error::errorId("lux.render.device.work_stopped");

    [[nodiscard]] LUX_RENDER_PUBLIC std::span<const error::ErrorDescriptor> deviceErrorDescriptors() noexcept;

    // Shared by Native submissions and Runtime delivery; preserve the existing public error identity.
    inline constexpr auto kOutcomeUnknown = error::errorId("lux.render.runtime.outcome_unknown");

    inline constexpr auto kCpuStopped = error::errorId("lux.render.resource.cpu_stopped");

    inline constexpr auto kInvalidTargetExtent = error::errorId("lux.render.core.invalid_target_extent");
    inline constexpr auto kInvalidTargetFormat = error::errorId("lux.render.core.invalid_target_format");
    inline constexpr auto kInvalidTargetSamples = error::errorId("lux.render.core.invalid_target_samples");
    inline constexpr auto kInvalidTargetKind = error::errorId("lux.render.core.invalid_target_kind");

    inline constexpr auto kInvalidViewport = error::errorId("lux.render.core.invalid_viewport");

    // Static descriptors for application registration. Core does not own a registry or log sink.
    [[nodiscard]] LUX_RENDER_PUBLIC std::span<const error::ErrorDescriptor> renderCoreErrorDescriptors() noexcept;
} // namespace lux::render
