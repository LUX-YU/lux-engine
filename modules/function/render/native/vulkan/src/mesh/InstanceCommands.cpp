#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshInstanceCommandHandler.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshInstanceStorage.hpp>

namespace lux::render::vulkan
{
    template <class Response> static exec::task<Response> rejected(RenderError error) noexcept
    {
        co_return cxx::unexpected(error);
    }

    exec::task<RenderResult<MeshInstanceId>> MeshInstanceCommandHandler::createInstanceSource(
        CreateMeshInstance::Request&& input
    ) noexcept
    {
        auto storage = device_.resources().borrow<MeshInstanceStorage>(input.storage);
        if (!storage)
        {
            return rejected<RenderResult<MeshInstanceId>>(storage.error());
        }
        return (*storage)->create(std::move(input.description));
    }

    exec::task<RenderResult<std::vector<MeshInstanceId>>> MeshInstanceCommandHandler::createInstancesSource(
        CreateMeshInstances::Request&& input
    ) noexcept
    {
        auto storage = device_.resources().borrow<MeshInstanceStorage>(input.storage);
        if (!storage)
        {
            return rejected<RenderResult<std::vector<MeshInstanceId>>>(storage.error());
        }
        return (*storage)->create(std::move(input.descriptions));
    }

    exec::task<RenderResult<TransformApplied>> MeshInstanceCommandHandler::updateTransformsSource(
        UpdateTransforms::Request&& input
    ) noexcept
    {
        auto storage = device_.resources().borrow<MeshInstanceStorage>(input.storage);
        if (!storage)
        {
            return rejected<RenderResult<TransformApplied>>(storage.error());
        }
        return (*storage)->update(std::move(input));
    }

    exec::task<RenderResult<TransformApplied>> MeshInstanceCommandHandler::updateInstancesSource(
        UpdateMeshInstances::Request&& input
    ) noexcept
    {
        auto storage = device_.resources().borrow<MeshInstanceStorage>(input.storage);
        if (!storage)
        {
            return rejected<RenderResult<TransformApplied>>(storage.error());
        }
        return (*storage)->update(std::move(input));
    }

    exec::task<RenderResult<Ack>> MeshInstanceCommandHandler::releaseInstanceSource(ReleaseMeshInstance::Request&& input
    ) noexcept
    {
        auto storage = device_.resources().borrow<MeshInstanceStorage>(input.storage);
        if (!storage)
        {
            return rejected<RenderResult<Ack>>(storage.error());
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
