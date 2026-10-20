#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/storage/RasterMaterialTableCommandHandler.hpp>
#include <lux/engine/render/storage/RasterMaterialTableStorage.hpp>

namespace lux::render::detail
{
    template <class Response> static exec::task<Response> rejected(RenderError error) noexcept
    {
        co_return cxx::unexpected(error);
    }

    exec::task<RenderResult<MaterialId>> RasterMaterialTableCommandHandler::createMaterialSource(
        CreateMaterial::Request&& input
    ) noexcept
    {
        auto storage = device_.resources().borrow<RasterMaterialTableStorage>(input.storage);
        if (!storage)
        {
            return rejected<RenderResult<MaterialId>>(storage.error());
        }
        return render::upload(**storage, std::move(input));
    }

    exec::task<RenderResult<Ack>> RasterMaterialTableCommandHandler::updateMaterialSource(
        UpdateMaterial::Request&& input
    ) noexcept
    {
        auto storage = device_.resources().borrow<RasterMaterialTableStorage>(input.material.storage);
        if (!storage)
        {
            return rejected<RenderResult<Ack>>(storage.error());
        }
        return render::upload(**storage, std::move(input));
    }

    void RasterMaterialTableCommandHandler::releaseMaterial(
        const ReleaseMaterial::Request& input,
        ReleaseMaterial::Response& response
    ) noexcept
    {
        auto storage = device_.resources().borrow<RasterMaterialTableStorage>(input.storage);
        if (!storage)
        {
            response.error = storage.error();
            return;
        }
        const auto released = (*storage)->release(input);
        response.error = released ? RenderError{} : released.error();
    }

} // namespace lux::render::detail

#include <lux/engine/render/runtime/Registration.hpp>

namespace lux::render
{
    template <>
    RenderResult<void> RenderServer::prepareStorage<RasterMaterialTableStorageDescriptor>(
        RegistrationBatch& batch,
        ConfigValue configuration,
        bool use_existing,
        std::string_view handler_key
    ) noexcept
    {
        return prepareStorageDefinition<
            RasterMaterialTableStorageDescriptor,
            detail::RasterMaterialTableStorage,
            detail::RasterMaterialTableCommandHandler>(
            batch,
            detail::materialStorageFactory(),
            std::move(configuration),
            use_existing,
            handler_key
        );
    }

    template LUX_RENDER_PUBLIC RenderResult<StorageTypeId> RenderServer::registerStorage<
        RasterMaterialTableStorageDescriptor>(std::string_view) noexcept;
} // namespace lux::render
