#include <lux/engine/render/runtime/Registration.hpp>
#include <lux/engine/render/storage/DirectLightingCommandHandler.hpp>
#include <lux/engine/render/storage/DirectLightingStorage.hpp>

namespace lux::render::detail
{
    template <class Value> static exec::task<RenderResult<Value>> rejected(RenderError error) noexcept
    {
        co_return cxx::unexpected(error);
    }

    exec::task<RenderResult<std::vector<LightId>>> DirectLightingCommandHandler::createSource(
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

    exec::task<RenderResult<std::uint32_t>> DirectLightingCommandHandler::updateSource(UpdateDirectLights::Request input
    ) noexcept
    {
        auto storage = resources_.borrow<DirectLightingStorage>(input.storage);
        if (!storage)
        {
            return rejected<std::uint32_t>(storage.error());
        }
        return (*storage)->update(input);
    }

    exec::task<RenderResult<Ack>> DirectLightingCommandHandler::removeSource(LightId input) noexcept
    {
        auto storage = resources_.borrow<DirectLightingStorage>(input.storage);
        if (!storage)
        {
            return rejected<Ack>(storage.error());
        }
        return (*storage)->remove(input);
    }
} // namespace lux::render::detail

namespace lux::render
{
    template <>
    RenderResult<void> RenderServer::prepareStorage<DirectLightingStorageDescriptor>(
        RegistrationBatch& batch,
        ConfigValue config,
        bool use_existing,
        std::string_view key
    ) noexcept
    {
        return prepareStorageDefinition<
            DirectLightingStorageDescriptor,
            detail::DirectLightingStorage,
            detail::DirectLightingCommandHandler>(
            batch,
            detail::directLightingStorageFactory(),
            std::move(config),
            use_existing,
            key
        );
    }

    template LUX_RENDER_PUBLIC RenderResult<StorageTypeId> RenderServer::registerStorage<
        DirectLightingStorageDescriptor>(std::string_view) noexcept;
} // namespace lux::render
