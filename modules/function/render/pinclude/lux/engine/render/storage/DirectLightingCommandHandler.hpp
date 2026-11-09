#pragma once

#include <lux/engine/render/storage/DirectLightingStorage.hpp>

#include <lux/engine/render/commands/Lighting3dCommands.hpp>

#include <exec/task.hpp>
#include <lux/engine/render/extensions/storage/DirectLighting.hpp>

namespace lux::render
{
    class ResourceRegistry;
}

namespace lux::render::detail
{
    class DirectLightingStorage;

    inline auto createLightResponse(std::uint64_t sequence) noexcept
    {
        return [sequence](RenderResult<std::vector<LightId>> result) noexcept
        {
            Lighting3dCommands::CreateLights::Response response;
            response.sequence = sequence;
            if (result)
            {
                response.lights = std::move(*result);
            }
            else
            {
                response.error = result.error();
            }
            return response;
        };
    }

    inline auto removeLightResponse(std::uint64_t sequence) noexcept
    {
        return [sequence](RenderResult<void> result) noexcept
        { return Lighting3dCommands::ReleaseLight::Response{result ? RenderError{} : result.error(), sequence}; };
    }

    class DirectLightingCommandHandler final
    {
    public:
        // Stateless typed methods; the Server supplies the selected Storage at dispatch.
        static DirectLightingCommandHandler* create(ResourceRegistry&) noexcept
        {
            return new DirectLightingCommandHandler;
        }

        using CreateSender = decltype(stdexec::then(
            std::declval<exec::task<RenderResult<std::vector<LightId>>>>(),
            createLightResponse(0)
        ));
        using RemoveSender =
            decltype(stdexec::then(std::declval<exec::task<RenderResult<void>>>(), removeLightResponse(0)));

        static constexpr std::string_view name = "lux.render.storage.DirectLightingCommandHandler";

        CreateSender
        createLights(DirectLightingStorage& storage, Lighting3dCommands::CreateLights::Request&& input) noexcept
        {
            const auto sequence = input.data.sequence;
            return storage.create(std::move(input.data)) | stdexec::then(createLightResponse(sequence));
        }

        static RenderResult<void> applied(RenderResult<std::uint32_t> result) noexcept
        {
            return result ? RenderResult<void>{} : cxx::unexpected(result.error());
        }

        RenderResult<void> updateLights(DirectLightingStorage& destination, LightUpdates input) noexcept
        {
            return applied(destination.update(input));
        }

        RemoveSender
        removeLight(DirectLightingStorage& storage, Lighting3dCommands::ReleaseLight::Request&& input) noexcept
        {
            return storage.remove(input.light) | stdexec::then(removeLightResponse(input.sequence));
        }
    };
} // namespace lux::render::detail
