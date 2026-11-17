#pragma once

#include <lux/engine/render/inputs/CameraInputs.hpp>

#include <lux/engine/render/commands/Command.hpp>

namespace lux::render
{
    struct CreateViewRequest final
    {
        SceneId scene{};
    };

    struct ViewCreated final
    {
        RenderError error{};
        ViewId view{};
    };

    struct CreateView final
    {
        static constexpr std::string_view name{"render.view.create"};
        static constexpr bool requires_frame_boundary = false;
        using Request = CreateViewRequest;
        using Response = ViewCreated;
    };

    struct RemoveView final
    {
        static constexpr bool failure_safe = true;
        static constexpr std::string_view name{"render.view.remove"};
        static constexpr bool requires_frame_boundary = true;
        using Request = ViewRequest;
        using Response = AckResponse;
    };
    struct UpdateViewCamera final
    {
        static constexpr std::string_view name = "render.view.update_camera";
        static constexpr bool requires_frame_boundary = true;
        struct Request final
        {
            ViewRequest view;
            CameraInput camera;
        };
        using Response = AckResponse;
    };
} // namespace lux::render
