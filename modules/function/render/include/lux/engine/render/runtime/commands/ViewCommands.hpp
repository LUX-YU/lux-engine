#pragma once

#include <lux/engine/render/runtime/commands/Command.hpp>
#include <lux/engine/render/resource/view/Parameters.hpp>

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

    struct UpdateView final
    {
        static constexpr std::string_view name{"render.view.update"};

        struct Request final
        {
            ViewScope scope;
            ViewGpuParameters parameters;
        };
        using Response = AckResponse;
    };

    struct RemoveView final
    {
        static constexpr bool failure_safe = true;
        static constexpr std::string_view name{"render.view.remove"};
        using Request = ViewScope;
        using Response = AckResponse;
    };
} // namespace lux::render
