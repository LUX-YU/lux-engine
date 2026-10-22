#include <lux/engine/render/storage/DirectLightingCommandHandler.hpp>
#include <lux/engine/render/storage/DirectLightingStorage.hpp>

namespace lux::render::detail
{
    template <class Value> static exec::task<RenderResult<Value>> rejected(RenderError error) noexcept
    {
        co_return cxx::unexpected(error);
    }

    exec::task<RenderResult<std::vector<LightId>>> DirectLightingCommandHandler::uploadLights(
        CreateDirectLights::Request input
    ) noexcept
    {
        auto storage = resources_.borrow<DirectLightingStorage>(input.storage);
        if (!storage)
        {
            return rejected<std::vector<LightId>>(storage.error());
        }
        return (*storage)->create(input);
    }

    exec::task<RenderResult<std::uint32_t>> DirectLightingCommandHandler::applyLightUpdates(
        UpdateDirectLights::Request input
    ) noexcept
    {
        auto storage = resources_.borrow<DirectLightingStorage>(input.storage);
        if (!storage)
        {
            return rejected<std::uint32_t>(storage.error());
        }
        return (*storage)->update(input);
    }

    exec::task<RenderResult<Ack>> DirectLightingCommandHandler::releaseLight(LightId input) noexcept
    {
        auto storage = resources_.borrow<DirectLightingStorage>(input.storage);
        if (!storage)
        {
            return rejected<Ack>(storage.error());
        }
        return (*storage)->remove(input);
    }
} // namespace lux::render::detail
