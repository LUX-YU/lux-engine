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
        // Local to the SceneId supplied in CreateViewRequest.
        ViewId view{};
    };

    struct CreateView final
    {
        static constexpr std::string_view name{"render.view.create"};
        using Request = CreateViewRequest;
        using Response = ViewCreated;
    };

    struct RemoveView final
    {
        static constexpr bool failure_safe = true;
        static constexpr std::string_view name{"render.view.remove"};
        using Request = ViewRequest;
        using Response = AckResponse;
    };
    struct UpdateViewCamera final
    {
        static constexpr std::string_view name = "render.view.update_camera";
        struct Request final
        {
            ViewRequest view;
            CameraInput camera;
        };
        using Response = AckResponse;
    };
} // namespace lux::render
