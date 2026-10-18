#include <lux/engine/render/native/vulkan/material/MaterialCommandHandler.hpp>
#include <lux/engine/render/native/vulkan/material/MaterialStorage.hpp>

namespace lux::render::vulkan
{
    GpuWorkSender<SetMaterial::Response> MaterialCommandHandler::setMaterial(SetMaterial::Request&& input) noexcept
    {
        auto storage = device_.resources().borrow<MaterialStorage>(input.material.storage);
        if (!storage)
        {
            return GpuWorkSender<SetMaterial::Response>{storage.error()};
        }
        auto candidate = render::upload(**storage, input);
        if (!candidate)
        {
            return GpuWorkSender<SetMaterial::Response>{candidate.error()};
        }
        return {std::move(candidate->first), std::move(candidate->second)};
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
