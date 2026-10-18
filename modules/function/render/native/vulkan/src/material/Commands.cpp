#include <lux/engine/render/native/vulkan/material/MaterialCommandHandler.hpp>
#include <lux/engine/render/native/vulkan/material/MaterialStorage.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>

namespace lux::render::vulkan
{
    static exec::task<SetMaterial::Response> rejectedMaterial(RenderError error) noexcept
    {
        co_return cxx::unexpected(error);
    }

    exec::task<SetMaterial::Response> MaterialCommandHandler::setMaterial(SetMaterial::Request&& input) noexcept
    {
        auto storage = device_.resources().borrow<MaterialStorage>(input.material.storage);
        if (!storage) return rejectedMaterial(storage.error());
        return render::upload(**storage, std::move(input));
    }

    void MaterialCommandHandler::queryMaterial(
        const QueryMaterial::Request& input,
        QueryMaterial::Response& response
    ) noexcept
    {
        auto storage = device_.resources().tryGet<render::MaterialStorage>(input.storage);
        if (!storage)
        {
            response = cxx::unexpected(storage.error());
            return;
        }
        auto result = storage->query(input);
        if (!result)
        {
            response = cxx::unexpected(result.error());
            return;
        }
        response = std::move(*result);
    }

    void MaterialCommandHandler::releaseMaterial(
        const ReleaseMaterial::Request& input,
        ReleaseMaterial::Response& response
    ) noexcept
    {
        auto storage = device_.resources().borrow<MaterialStorage>(input.material.storage);
        if (!storage)
        {
            response = cxx::unexpected(storage.error());
            return;
        }
        if (auto result = (*storage)->release(input.material, input.revision); !result)
        {
            response = cxx::unexpected(result.error());
            return;
        }
        response = Ack{};
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
