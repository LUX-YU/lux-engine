#pragma once

#include <lux/engine/render/runtime/commands/Command.hpp>
#include <lux/engine/render/resource/texture/Data.hpp>

namespace lux::render::TextureCommands
{
    // Commands select the owning Resource instance; capability implementation selection does not route uploads.
    struct Upload final
    {
        static constexpr std::string_view name = "lux.render.image.upload";
        using Data = TextureData;

        struct Request final
        {
            ResourceInstanceId resource_id;
            Data data;
        };

        [[nodiscard]] static ResourceInstanceId resourceId(const Request& request) noexcept
        {
            return request.resource_id;
        }

        // Success follows Copy/Acquire acceptance and publication, not a blocking GPU wait.
        struct Response final
        {
            RenderError error{};
            TextureId texture{};
        };
    };

    struct CloneRegion final
    {
        static constexpr std::string_view name = "lux.render.image.clone_region";
        using Data = TexturePatchData;
        // The source texture and source bytes remain valid until the terminal Response.
        struct Request final
        {
            Data data;
        };

        [[nodiscard]] static TextureId resourceId(const Request& request) noexcept
        {
            return request.data.texture;
        }

        struct Response final
        {
            RenderError error{};
            TextureId texture{};
        };
    };

    struct Release final
    {
        static constexpr std::string_view name = "lux.render.image.release";
        static constexpr bool requires_idle_resource = true;
        static constexpr bool failure_safe = true;
        using Data = TextureId;
        // Client ends Material/Attachment references first. Active source clones reject release as busy.
        struct Request final
        {
            Data data;
        };

        [[nodiscard]] static TextureId resourceId(const Request& request) noexcept
        {
            return request.data;
        }

        using Response = AckResponse;
    };
} // namespace lux::render::TextureCommands
