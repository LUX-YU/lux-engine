#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/storage/RasterMaterialTableCommandHandler.hpp>
#include <lux/engine/render/storage/RasterMaterialTableStorage.hpp>

namespace lux::render::detail
{
    template <class Response> static exec::task<Response> rejected(RenderError error) noexcept
    {
        co_return cxx::unexpected(error);
    }

    exec::task<RenderResult<MaterialId>> RasterMaterialTableCommandHandler::uploadMaterial(
        CreateMaterial::Request&& input
    ) noexcept
    {
        auto storage = resources_.borrowInput<RasterMaterialTableStorage>(input.destination);
        if (!storage)
        {
            return rejected<RenderResult<MaterialId>>(storage.error());
        }
        return render::upload(**storage, std::move(input));
    }

    exec::task<RenderResult<Ack>> RasterMaterialTableCommandHandler::uploadMaterialParameters(
        UpdateMaterial::Request&& input
    ) noexcept
    {
        auto storage =
            resources_.borrow<RasterMaterialTableStorage>(extension_detail::ResourceIdentity::storage(input.material));
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
        auto storage =
            resources_.borrow<RasterMaterialTableStorage>(extension_detail::ResourceIdentity::storage(input));
        if (!storage)
        {
            response.error = storage.error();
            return;
        }
        const auto released = (*storage)->release(input);
        response.error = released ? RenderError{} : released.error();
    }

} // namespace lux::render::detail
