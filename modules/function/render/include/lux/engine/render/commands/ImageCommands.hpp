#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/inputs/SampledTextureInputs.hpp>

namespace lux::render::ImageCommands
{
    // Shared by visible capabilities that consume sampled images; no Storage capability is exposed.
    struct Upload final
    {
        static constexpr std::string_view name = "lux.render.image.upload";
        static constexpr bool requires_frame_boundary = false;
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
        static constexpr bool requires_frame_boundary = false;
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
        static constexpr bool requires_frame_boundary = true;
        static constexpr bool failure_safe = true;
        using Data = TextureId;
        // Client ends Material/Attachment references first. No implicit cross-Storage updates.
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
} // namespace lux::render::ImageCommands
