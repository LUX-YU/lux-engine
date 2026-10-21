#pragma once

#include <exec/task.hpp>
#include <lux/engine/render/commands/CommandMethod.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <lux/engine/render/storage/DirectLighting.hpp>

namespace lux::render::detail
{
    inline auto createLightResponse(std::uint64_t sequence) noexcept
    {
        return [sequence](RenderResult<std::vector<LightId>> result) noexcept
        {
            CreateDirectLights::Response response;
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

    inline auto updateLightResponse(std::uint64_t sequence) noexcept
    {
        return [sequence](RenderResult<std::uint32_t> result) noexcept
        {
            return UpdateDirectLights::Response{
                result ? RenderError{} : result.error(),
                sequence,
                result ? *result : 0
            };
        };
    }

    inline auto removeLightResponse(std::uint64_t sequence) noexcept
    {
        return [sequence](RenderResult<Ack> result) noexcept
        { return RemoveDirectLight::Response{result ? RenderError{} : result.error(), sequence}; };
    }

    class DirectLightingCommandHandler final
    {
    public:
        using CreateSender = decltype(stdexec::then(
            std::declval<exec::task<RenderResult<std::vector<LightId>>>>(),
            createLightResponse(0)
        ));
        using UpdateSender =
            decltype(stdexec::then(std::declval<exec::task<RenderResult<std::uint32_t>>>(), updateLightResponse(0)));
        using RemoveSender =
            decltype(stdexec::then(std::declval<exec::task<RenderResult<Ack>>>(), removeLightResponse(0)));

        static constexpr std::string_view name = "lux.render.storage.DirectLightingCommandHandler";

        explicit DirectLightingCommandHandler(ResourceRegistry& resources) noexcept : resources_(resources) {}

        CreateSender create(CreateDirectLights::Request&& input) noexcept
        {
            const auto sequence = input.sequence;
            return createSource(input) | stdexec::then(createLightResponse(sequence));
        }

        UpdateSender update(UpdateDirectLights::Request&& input) noexcept
        {
            const auto sequence = input.sequence;
            return updateSource(input) | stdexec::then(updateLightResponse(sequence));
        }

        RemoveSender remove(RemoveDirectLight::Request&& input) noexcept
        {
            return removeSource(input.light) | stdexec::then(removeLightResponse(input.sequence));
        }

        using Bindings = std::tuple<
            CommandMethod<CreateDirectLights, &DirectLightingCommandHandler::create>,
            CommandMethod<UpdateDirectLights, &DirectLightingCommandHandler::update>,
            CommandMethod<RemoveDirectLight, &DirectLightingCommandHandler::remove>>;

    private:
        exec::task<RenderResult<std::vector<LightId>>> createSource(CreateDirectLights::Request) noexcept;
        exec::task<RenderResult<std::uint32_t>> updateSource(UpdateDirectLights::Request) noexcept;
        exec::task<RenderResult<Ack>> removeSource(LightId) noexcept;
        ResourceRegistry& resources_;
    };
} // namespace lux::render::detail
