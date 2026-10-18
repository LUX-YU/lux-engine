#include <lux/engine/render/native/vulkan/mesh/MeshInstanceCommandHandler.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshInstanceStorage.hpp>

namespace lux::render::vulkan
{
    void MeshInstanceCommandHandler::createInstance(
        const CreateMeshInstance::Request& input,
        CreateMeshInstance::Response& response
    ) noexcept
    {
        auto storage = device_.resources().borrow<MeshInstanceStorage>(input.storage);
        if (!storage)
        {
            response = cxx::unexpected(storage.error());
            return;
        }
        auto result = (*storage)->create(input.description);
        if (!result)
        {
            response = cxx::unexpected(result.error());
            return;
        }
        response = std::move(*result);
    }

    void MeshInstanceCommandHandler::updateTransforms(
        const UpdateTransforms::Request& input,
        UpdateTransforms::Response& response
    ) noexcept
    {
        auto storage = device_.resources().borrow<MeshInstanceStorage>(input.storage);
        if (!storage)
        {
            response = cxx::unexpected(storage.error());
            return;
        }
        auto result = (*storage)->update(input);
        if (!result)
        {
            response = cxx::unexpected(result.error());
            return;
        }
        response = std::move(*result);
    }

    void MeshInstanceCommandHandler::queryInstance(
        const QueryMeshInstance::Request& input,
        QueryMeshInstance::Response& response
    ) noexcept
    {
        auto storage = device_.resources().tryGet<render::MeshInstanceStorage>(input.storage);
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

    void MeshInstanceCommandHandler::releaseInstance(
        const ReleaseMeshInstance::Request& input,
        ReleaseMeshInstance::Response& response
    ) noexcept
    {
        auto storage = device_.resources().borrow<MeshInstanceStorage>(input.instance.storage);
        if (!storage)
        {
            response = cxx::unexpected(storage.error());
            return;
        }
        if (auto result = (*storage)->release(input); !result)
        {
            response = cxx::unexpected(result.error());
            return;
        }
        response = Ack{};
    }

} // namespace lux::render::vulkan

namespace lux::render
{
    StorageFactory StorageProvider<render::MeshInstanceStorage>::factory() noexcept
    {
        return vulkan::meshInstanceStorageFactory();
    }

    CppType StorageProvider<render::MeshInstanceStorage>::implementationType() noexcept
    {
        return cppType<Storage>();
    }
} // namespace lux::render
