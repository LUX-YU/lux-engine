#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshInstanceCommandHandler.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshInstanceStorage.hpp>

namespace lux::render::vulkan
{
    template <class Response> static exec::task<Response> rejected(RenderError error) noexcept
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
            return rejected<CreateMeshInstance::Response>(storage.error());
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
            return rejected<UpdateTransforms::Response>(storage.error());
        }
        return (*storage)->update(std::move(input));
    }

    exec::task<UpdateMeshInstances::Response> MeshInstanceCommandHandler::updateInstances(
        UpdateMeshInstances::Request&& input
    ) noexcept
    {
        auto storage = device_.resources().borrow<MeshInstanceStorage>(input.storage);
        if (!storage)
        {
            return rejected<UpdateMeshInstances::Response>(storage.error());
        }
        return (*storage)->update(std::move(input));
    }

    exec::task<ReleaseMeshInstance::Response> MeshInstanceCommandHandler::releaseInstance(
        ReleaseMeshInstance::Request&& input
    ) noexcept
    {
        auto storage = device_.resources().borrow<MeshInstanceStorage>(input.storage);
        if (!storage)
        {
            return rejected<ReleaseMeshInstance::Response>(storage.error());
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
