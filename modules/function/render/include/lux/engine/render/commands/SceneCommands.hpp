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

    // A scene stays bound to the device hosting this Server for its entire lifetime.
    struct CreateScene final
    {
        static constexpr std::string_view name{"render.scene.create"};
        using Request = EmptyRequest;
        using Response = SceneCreated;
    };

    struct RemoveScene final
    {
        static constexpr bool failure_safe = true;
        static constexpr std::string_view name{"render.scene.remove"};
        using Request = SceneRequest;
        using Response = AckResponse;
    };
} // namespace lux::render
