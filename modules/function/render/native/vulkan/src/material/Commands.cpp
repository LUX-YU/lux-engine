#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/material/MaterialCommandHandler.hpp>
#include <lux/engine/render/native/vulkan/material/MaterialStorage.hpp>

namespace lux::render::vulkan
{
    template <class Response> static exec::task<Response> rejected(RenderError error) noexcept
    {
        co_return cxx::unexpected(error);
    }

    exec::task<RenderResult<MaterialId>> MaterialCommandHandler::createMaterialSource(CreateMaterial::Request&& input
    ) noexcept
    {
        auto storage = device_.resources().borrow<MaterialStorage>(input.storage);
        if (!storage)
        {
            return rejected<RenderResult<MaterialId>>(storage.error());
        }
        return render::upload(**storage, std::move(input));
    }

    exec::task<RenderResult<Ack>> MaterialCommandHandler::updateMaterialSource(UpdateMaterial::Request&& input) noexcept
    {
        auto storage = device_.resources().borrow<MaterialStorage>(input.material.storage);
        if (!storage)
        {
            return rejected<RenderResult<Ack>>(storage.error());
        }
        return render::upload(**storage, std::move(input));
    }

    void MaterialCommandHandler::releaseMaterial(
        const ReleaseMaterial::Request& input,
        ReleaseMaterial::Response& response
    ) noexcept
    {
        auto storage = device_.resources().borrow<MaterialStorage>(input.storage);
        if (!storage)
        {
            response.error = storage.error();
            return;
        }
        const auto released = (*storage)->release(input);
        response.error = released ? RenderError{} : released.error();
    }

} // namespace lux::render::vulkan

namespace lux::render
{
    StorageFactory StorageProvider<render::MaterialStorage>::factory() noexcept
    {
        return vulkan::materialStorageFactory();
    }

    CppType StorageProvider<render::MaterialStorage>::implementationType() noexcept
    {
        return cppType<Storage>();
    }
} // namespace lux::render
