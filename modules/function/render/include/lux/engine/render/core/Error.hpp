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
    inline constexpr auto kNativeThreadStart = error::errorId("lux.render.native.thread_start");
    inline constexpr auto kNativeExecutionClosed = error::errorId("lux.render.native.closed");

    inline constexpr auto kDeviceBusy = error::errorId("lux.render.device.busy");
    inline constexpr auto kUploadStopped = error::errorId("lux.render.device.work_stopped");

    [[nodiscard]] LUX_RENDER_PUBLIC std::span<const error::ErrorDescriptor> deviceErrorDescriptors() noexcept;

    // Shared by Native submissions and Runtime delivery; preserve the existing public error identity.
    inline constexpr auto kOutcomeUnknown = error::errorId("lux.render.runtime.outcome_unknown");

    inline constexpr auto kCpuStopped = error::errorId("lux.render.resource.cpu_stopped");

    // Command callers can inspect resource failures without depending on Storage authoring contracts.
    inline constexpr auto kStorageContract = error::errorId("lux.render.resource.contract");
    inline constexpr auto kStorageMismatch = error::errorId("lux.render.resource.mismatch");
    inline constexpr auto kStorageFactory = error::errorId("lux.render.resource.factory");
    inline constexpr auto kStorageCapability = error::errorId("lux.render.resource.capability");
    inline constexpr auto kStorageIdentity = error::errorId("lux.render.resource.identity");
    inline constexpr auto kStorageCapacity = error::errorId("lux.render.resource.capacity");

    inline constexpr auto kInvalidTargetExtent = error::errorId("lux.render.core.invalid_target_extent");
    inline constexpr auto kInvalidTargetFormat = error::errorId("lux.render.core.invalid_target_format");
    inline constexpr auto kInvalidTargetSamples = error::errorId("lux.render.core.invalid_target_samples");
    inline constexpr auto kInvalidTargetKind = error::errorId("lux.render.core.invalid_target_kind");

    inline constexpr auto kInvalidViewport = error::errorId("lux.render.core.invalid_viewport");

    inline constexpr auto kCapabilityDeclaration = error::errorId("lux.render.capability");
    inline constexpr auto kCapabilityConflict = error::errorId("lux.render.capability.conflict");
    inline constexpr auto kCapabilityPrerequisite = error::errorId("lux.render.capability.prerequisite");
    inline constexpr auto kCapabilityCycle = error::errorId("lux.render.capability.cycle");
    inline constexpr auto kCapabilityInUse = error::errorId("lux.render.capability.in_use");

    // Static descriptors for application registration. Core does not own a registry or log sink.
    [[nodiscard]] LUX_RENDER_PUBLIC std::span<const error::ErrorDescriptor> renderCoreErrorDescriptors() noexcept;
    // Complete immutable diagnostic catalog for application outlet assembly. No Graph/Storage
    // author headers are needed to format failures received from ordinary commands.
    [[nodiscard]] LUX_RENDER_PUBLIC std::span<const std::span<const error::ErrorDescriptor>>
    renderErrorDescriptorTables() noexcept;
} // namespace lux::render
