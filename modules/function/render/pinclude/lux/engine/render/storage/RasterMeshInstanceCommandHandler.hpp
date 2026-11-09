#pragma once

#include <lux/engine/render/storage/RasterMeshInstanceStorage.hpp>

#include <lux/engine/render/commands/Mesh3dCommands.hpp>

#include <exec/task.hpp>

#include <lux/engine/render/extensions/storage/RasterMeshInstances.hpp>
#include <lux/engine/render/extensions/storage/StorageFactory.hpp>

namespace lux::render
{
    class ResourceRegistry;
}

namespace lux::render::detail
{
    class DeviceSession;
    class RasterMeshInstanceStorage;

    class RasterMeshInstanceCommandHandler final
    {
    private:
        static constexpr auto create_instance_response_ = [](RenderResult<MeshInstanceId> result
                                                          ) noexcept -> Mesh3dCommands::CreateInstance::Response
        {
            if (!result)
            {
                return {result.error()};
            }
            return {{}, *result};
        };

        static constexpr auto create_instances_response_ = [](RenderResult<std::vector<MeshInstanceId>> result
                                                           ) noexcept -> Mesh3dCommands::CreateInstances::Response
        {
            if (!result)
            {
                return {result.error()};
            }
            return {{}, std::move(*result)};
        };

        static constexpr auto release_instance_response_ = [](RenderResult<Ack> result
                                                           ) noexcept -> Mesh3dCommands::ReleaseInstance::Response
        { return {result ? RenderError{} : result.error()}; };

    public:
        // Stateless typed methods; the Server supplies the selected Storage at dispatch.
        static RasterMeshInstanceCommandHandler* create(ResourceRegistry&) noexcept
        {
            return new RasterMeshInstanceCommandHandler;
        }

        using CreateInstanceSender =
            decltype(stdexec::then(std::declval<exec::task<RenderResult<MeshInstanceId>>>(), create_instance_response_)
            );

        using CreateInstancesSender = decltype(stdexec::then(
            std::declval<exec::task<RenderResult<std::vector<MeshInstanceId>>>>(),
            create_instances_response_
        ));

        using ReleaseInstanceSender =
            decltype(stdexec::then(std::declval<exec::task<RenderResult<Ack>>>(), release_instance_response_));

        static constexpr std::string_view name = "lux.render.storage.RasterMeshInstanceCommandHandler";

        CreateInstanceSender
        createInstance(RasterMeshInstanceStorage& storage, Mesh3dCommands::CreateInstance::Request&& input) noexcept
        {
            return storage.create(std::move(input.data)) | stdexec::then(create_instance_response_);
        }

        CreateInstancesSender
        createInstances(RasterMeshInstanceStorage& storage, Mesh3dCommands::CreateInstances::Request&& input) noexcept
        {
            return storage.create(std::move(input.data)) | stdexec::then(create_instances_response_);
        }

        static RenderResult<void> applied(RenderResult<TransformApplied> result) noexcept
        {
            return result ? RenderResult<void>{} : cxx::unexpected(result.error());
        }

        RenderResult<void> updateTransforms(RasterMeshInstanceStorage& destination, TransformData input) noexcept
        {
            return applied(destination.update(input));
        }

        RenderResult<void> updateInstances(RasterMeshInstanceStorage& destination, MeshInstanceUpdates input) noexcept
        {
            return applied(destination.update(input));
        }

        ReleaseInstanceSender
        releaseInstance(RasterMeshInstanceStorage& storage, Mesh3dCommands::ReleaseInstance::Request&& input) noexcept
        {
            return storage.release(input.data) | stdexec::then(release_instance_response_);
        }
    };

} // namespace lux::render::detail
