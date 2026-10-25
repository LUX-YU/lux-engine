#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/storage/RasterMeshInstanceCommandHandler.hpp>
#include <lux/engine/render/storage/RasterMeshInstanceStorage.hpp>

namespace lux::render::detail
{
    template <class Response> static exec::task<Response> rejected(RenderError error) noexcept
    {
        co_return cxx::unexpected(error);
    }

    exec::task<RenderResult<MeshInstanceId>> RasterMeshInstanceCommandHandler::uploadInstance(
        CreateMeshInstance::Request&& input
    ) noexcept
    {
        auto storage = resources_.borrowInput<RasterMeshInstanceStorage>(input.destination);
        if (!storage)
        {
            return rejected<RenderResult<MeshInstanceId>>(storage.error());
        }
        return (*storage)->create(std::move(input.description));
    }

    exec::task<RenderResult<std::vector<MeshInstanceId>>> RasterMeshInstanceCommandHandler::uploadInstances(
        CreateMeshInstances::Request&& input
    ) noexcept
    {
        auto storage = resources_.borrowInput<RasterMeshInstanceStorage>(input.destination);
        if (!storage)
        {
            return rejected<RenderResult<std::vector<MeshInstanceId>>>(storage.error());
        }
        return (*storage)->create(std::move(input.descriptions));
    }

    exec::task<RenderResult<TransformApplied>> RasterMeshInstanceCommandHandler::applyTransformUpdates(
        UpdateTransforms::Request&& input
    ) noexcept
    {
        auto storage = resources_.borrowInput<RasterMeshInstanceStorage>(input.destination);
        if (!storage)
        {
            return rejected<RenderResult<TransformApplied>>(storage.error());
        }
        return (*storage)->update(std::move(input));
    }

    exec::task<RenderResult<TransformApplied>> RasterMeshInstanceCommandHandler::applyInstanceUpdates(
        UpdateMeshInstances::Request&& input
    ) noexcept
    {
        auto storage = resources_.borrowInput<RasterMeshInstanceStorage>(input.destination);
        if (!storage)
        {
            return rejected<RenderResult<TransformApplied>>(storage.error());
        }
        return (*storage)->update(std::move(input));
    }

    exec::task<RenderResult<Ack>> RasterMeshInstanceCommandHandler::releaseStoredInstance(
        ReleaseMeshInstance::Request&& input
    ) noexcept
    {
        auto storage = resources_.borrow<RasterMeshInstanceStorage>(extension_detail::ResourceIdentity::storage(input));
        if (!storage)
        {
            return rejected<RenderResult<Ack>>(storage.error());
        }
        return (*storage)->release(input);
    }

} // namespace lux::render::detail
