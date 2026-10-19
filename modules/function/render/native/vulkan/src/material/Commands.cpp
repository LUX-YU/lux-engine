#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/material/MaterialCommandHandler.hpp>
#include <lux/engine/render/native/vulkan/material/MaterialStorage.hpp>

namespace lux::render::vulkan
{
    template <class Response> static exec::task<Response> rejected(RenderError error) noexcept
    {
        co_return cxx::unexpected(error);
    }

    exec::task<CreateMaterial::Response> MaterialCommandHandler::createMaterial(CreateMaterial::Request&& input
    ) noexcept
    {
        auto storage = device_.resources().borrow<MaterialStorage>(input.storage);
        if (!storage)
        {
            return rejected<CreateMaterial::Response>(storage.error());
        }
        return render::upload(**storage, std::move(input));
    }

    exec::task<UpdateMaterial::Response> MaterialCommandHandler::updateMaterial(UpdateMaterial::Request&& input
    ) noexcept
    {
        auto storage = device_.resources().borrow<MaterialStorage>(input.material.storage);
        if (!storage)
        {
            return rejected<UpdateMaterial::Response>(storage.error());
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
            response = cxx::unexpected(storage.error());
            return;
        }
        const auto released = (*storage)->release(input);
        response = released ? ReleaseMaterial::Response(Ack{}) : cxx::unexpected(released.error());
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
