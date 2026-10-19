#include <lux/engine/render/native/vulkan/mesh/MeshInstanceCommandHandler.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshInstanceStorage.hpp>

namespace lux::render::vulkan
{
    template <class Response> static exec::task<Response> rejectedInstance(RenderError error) noexcept
    {
        co_return cxx::unexpected(error);
    }

    exec::task<CreateMeshInstance::Response> MeshInstanceCommandHandler::createInstance(
        CreateMeshInstance::Request&& input
    ) noexcept
    {
        auto storage = device_.resources().borrow<MeshInstanceStorage>(input.storage);
        if (!storage)
        {
            return rejectedInstance<CreateMeshInstance::Response>(storage.error());
        }
        return (*storage)->create(std::move(input.description));
    }

    exec::task<UpdateTransforms::Response> MeshInstanceCommandHandler::updateTransforms(
        UpdateTransforms::Request&& input
    ) noexcept
    {
        auto storage = device_.resources().borrow<MeshInstanceStorage>(input.storage);
        if (!storage)
        {
            return rejectedInstance<UpdateTransforms::Response>(storage.error());
        }
        return (*storage)->update(std::move(input));
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

    exec::task<ReleaseMeshInstance::Response> MeshInstanceCommandHandler::releaseInstance(
        ReleaseMeshInstance::Request&& input
    ) noexcept
    {
        auto storage = device_.resources().borrow<MeshInstanceStorage>(input.instance.storage);
        if (!storage)
        {
            return rejectedInstance<ReleaseMeshInstance::Response>(storage.error());
        }
        return (*storage)->release(input);
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
