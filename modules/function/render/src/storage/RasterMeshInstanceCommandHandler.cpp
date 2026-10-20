#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/storage/RasterMeshInstanceCommandHandler.hpp>
#include <lux/engine/render/storage/RasterMeshInstanceStorage.hpp>

namespace lux::render::detail
{
    template <class Response> static exec::task<Response> rejected(RenderError error) noexcept
    {
        co_return cxx::unexpected(error);
    }

    exec::task<RenderResult<MeshInstanceId>> RasterMeshInstanceCommandHandler::createInstanceSource(
        CreateMeshInstance::Request&& input
    ) noexcept
    {
        auto storage = resources_.borrow<RasterMeshInstanceStorage>(input.storage);
        if (!storage)
        {
            return rejected<RenderResult<MeshInstanceId>>(storage.error());
        }
        return (*storage)->create(std::move(input.description));
    }

    exec::task<RenderResult<std::vector<MeshInstanceId>>> RasterMeshInstanceCommandHandler::createInstancesSource(
        CreateMeshInstances::Request&& input
    ) noexcept
    {
        auto storage = resources_.borrow<RasterMeshInstanceStorage>(input.storage);
        if (!storage)
        {
            return rejected<RenderResult<std::vector<MeshInstanceId>>>(storage.error());
        }
        return (*storage)->create(std::move(input.descriptions));
    }

    exec::task<RenderResult<TransformApplied>> RasterMeshInstanceCommandHandler::updateTransformsSource(
        UpdateTransforms::Request&& input
    ) noexcept
    {
        auto storage = resources_.borrow<RasterMeshInstanceStorage>(input.storage);
        if (!storage)
        {
            return rejected<RenderResult<TransformApplied>>(storage.error());
        }
        return (*storage)->update(std::move(input));
    }

    exec::task<RenderResult<TransformApplied>> RasterMeshInstanceCommandHandler::updateInstancesSource(
        UpdateMeshInstances::Request&& input
    ) noexcept
    {
        auto storage = resources_.borrow<RasterMeshInstanceStorage>(input.storage);
        if (!storage)
        {
            return rejected<RenderResult<TransformApplied>>(storage.error());
        }
        return (*storage)->update(std::move(input));
    }

    exec::task<RenderResult<Ack>> RasterMeshInstanceCommandHandler::releaseInstanceSource(
        ReleaseMeshInstance::Request&& input
    ) noexcept
    {
        auto storage = resources_.borrow<RasterMeshInstanceStorage>(input.storage);
        if (!storage)
        {
            return rejected<RenderResult<Ack>>(storage.error());
        }
        return (*storage)->release(input);
    }

} // namespace lux::render::detail

#include <lux/engine/render/runtime/Registration.hpp>

namespace lux::render
{
    template <>
    RenderResult<void> RenderServer::prepareStorage<RasterMeshInstanceStorageDescriptor>(
        RegistrationBatch& batch,
        ConfigValue configuration,
        bool use_existing,
        std::string_view handler_key
    ) noexcept
    {
        return prepareStorageDefinition<
            RasterMeshInstanceStorageDescriptor,
            detail::RasterMeshInstanceStorage,
            detail::RasterMeshInstanceCommandHandler>(
            batch,
            detail::meshInstanceStorageFactory(),
            std::move(configuration),
            use_existing,
            handler_key
        );
    }

    template LUX_RENDER_PUBLIC RenderResult<StorageTypeId> RenderServer::registerStorage<
        RasterMeshInstanceStorageDescriptor>(std::string_view) noexcept;
} // namespace lux::render
