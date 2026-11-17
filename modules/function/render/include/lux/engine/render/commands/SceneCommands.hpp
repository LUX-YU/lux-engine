#pragma once

#include <lux/engine/render/commands/Command.hpp>

namespace lux::render
{
    struct SceneRequest final
    {
        SceneId scene{};
    };

    struct SceneCreated final
    {
        RenderError error{};
        SceneId scene{};
    };

    struct CreateScene final
    {
        static constexpr std::string_view name{"render.scene.create"};
        static constexpr bool requires_frame_boundary = false;
        using Request = EmptyRequest;
        using Response = SceneCreated;
    };

    struct RemoveScene final
    {
        static constexpr bool failure_safe = true;
        static constexpr std::string_view name{"render.scene.remove"};
        static constexpr bool requires_frame_boundary = true;
        using Request = SceneRequest;
        using Response = AckResponse;
    };
} // namespace lux::render
