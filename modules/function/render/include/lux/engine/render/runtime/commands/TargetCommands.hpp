#pragma once

#include <lux/engine/render/runtime/TargetId.hpp>
#include <lux/engine/render/device/DeviceId.hpp>
#include <lux/engine/render/effect/EffectId.hpp>
#include <lux/engine/render/runtime/commands/Command.hpp>
#include <lux/engine/render/runtime/Target.hpp>
#include <optional>

namespace lux::render
{
    struct CreateTargetRequest final
    {
        DeviceSessionId device;
        TargetDesc description{};
        std::optional<SurfaceDescription> surface; // Required for a swapchain, absent for Offscreen.
    };

    struct ResizeTargetRequest final
    {
        TargetId target{};
        TargetDesc description{};
        // Replacement for the installed surface writer. Prepare the Effect on this device before reconfiguration.
        EffectId output_effect{};
        // A host display-change event can require a fresh WSI/display description even at the same extent.
        bool display_changed = false;
    };

    struct TargetCreated final
    {
        RenderError error{};
        TargetId target{};
        std::uint64_t epoch = 0;
        TargetDesc description; // Actual output format/extent selected by the backend.
        // Physical output still exists after this attempt; false after an unsuccessful WSI replacement.
        // This does not mean a new image has been rendered or presented.
        bool output_available{};
        // Display-referred SDR white for linear HDR composition. Zero when no HDR surface is available.
        float sdr_white_nits{};
    };

    struct CreateTarget final
    {
        static constexpr std::string_view name{"render.target.create"};
        using Request = CreateTargetRequest;
        using Response = TargetCreated;
    };

    // A mode switch with a replacement Effect retains the writer and its activation. Other resizes pause the Target.
    // Resume a paused Target with the epoch returned by ResizeTarget.
    struct ResizeTarget final
    {
        // If a WSI replacement fails after retiring the previous chain, output_available is false.
        // A later ResizeTarget may retry using the same TargetId.
        static constexpr std::string_view name{"render.target.resize"};
        using Request = ResizeTargetRequest;
        using Response = TargetCreated;
    };

    struct RemoveTarget final
    {
        static constexpr bool failure_safe = true;
        static constexpr std::string_view name = "render.target.remove";
        using Request = TargetId;
        using Response = AckResponse;
    };

    struct SetTargetActive final
    {
        static constexpr std::string_view name = "render.target.set_active";

        struct Request final
        {
            TargetId target;
            bool active = true;
            std::uint64_t epoch = 1; // Enabling requires the actual CreateTarget/ResizeTarget epoch.
        };

        using Response = AckResponse;
    };
} // namespace lux::render
