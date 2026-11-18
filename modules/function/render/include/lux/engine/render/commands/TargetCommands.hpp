#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <optional>

namespace lux::render
{
    struct CreateTargetRequest final
    {
        TargetDesc description{};
        std::optional<SurfaceDescription> surface; // Required for a swapchain, absent for Offscreen.
    };

    struct ResizeTargetRequest final
    {
        TargetId target{};
        TargetDesc description{};
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
    };

    struct CreateTarget final
    {
        static constexpr std::string_view name{"render.target.create"};
        using Request = CreateTargetRequest;
        using Response = TargetCreated;
    };

    // On success the Target is paused. Apply the replied dimensions and SetTargetActive(true)
    // together in the next complete frame; earlier queued frames cannot draw with stale size-dependent inputs.
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
