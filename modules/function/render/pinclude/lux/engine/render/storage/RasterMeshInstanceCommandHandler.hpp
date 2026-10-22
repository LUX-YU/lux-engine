#pragma once

#include <lux/engine/render/resource/ResourceRegistry.hpp>

#include <exec/task.hpp>
#include <lux/engine/render/commands/CommandMethod.hpp>

#include <lux/engine/render/resource/StorageFactory.hpp>
#include <lux/engine/render/storage/RasterMeshInstances.hpp>

namespace lux::render::detail
{
    class DeviceSession;
    class RasterMeshInstanceStorage;

    class RasterMeshInstanceCommandHandler final
    {
    private:
        static constexpr auto create_instance_response_ = [](RenderResult<MeshInstanceId> result
                                                          ) noexcept -> CreateMeshInstance::Response
        {
            if (!result)
            {
                return {result.error()};
            }
            return {{}, *result};
        };

        static constexpr auto create_instances_response_ = [](RenderResult<std::vector<MeshInstanceId>> result
                                                           ) noexcept -> CreateMeshInstances::Response
        {
            if (!result)
            {
                return {result.error()};
            }
            return {{}, std::move(*result)};
        };

        static constexpr auto update_transforms_response_ = [](RenderResult<TransformApplied> result
                                                            ) noexcept -> UpdateTransforms::Response
        {
            if (!result)
            {
                return {result.error()};
            }
            return {{}, result->sequence, result->changed};
        };

        static constexpr auto update_instances_response_ = [](RenderResult<TransformApplied> result
                                                           ) noexcept -> UpdateMeshInstances::Response
        {
            if (!result)
            {
                return {result.error()};
            }
            return {{}, result->sequence, result->changed};
        };

        static constexpr auto release_instance_response_ = [](RenderResult<Ack> result
                                                           ) noexcept -> ReleaseMeshInstance::Response
        { return {result ? RenderError{} : result.error()}; };

    public:
        // Factory registration does not construct an object. Activation borrows the one resource directory.
        static RasterMeshInstanceCommandHandler* create(ResourceRegistry& resources) noexcept
        {
            return new RasterMeshInstanceCommandHandler(resources);
        }

        using CreateInstanceSender =
            decltype(stdexec::then(std::declval<exec::task<RenderResult<MeshInstanceId>>>(), create_instance_response_)
            );

        using CreateInstancesSender = decltype(stdexec::then(
            std::declval<exec::task<RenderResult<std::vector<MeshInstanceId>>>>(),
            create_instances_response_
        ));

        using UpdateTransformsSender = decltype(stdexec::then(
            std::declval<exec::task<RenderResult<TransformApplied>>>(),
            update_transforms_response_
        ));

        using UpdateInstancesSender = decltype(stdexec::then(
            std::declval<exec::task<RenderResult<TransformApplied>>>(),
            update_instances_response_
        ));

        using ReleaseInstanceSender =
            decltype(stdexec::then(std::declval<exec::task<RenderResult<Ack>>>(), release_instance_response_));

        static constexpr std::string_view name = "lux.render.storage.RasterMeshInstanceCommandHandler";

        explicit RasterMeshInstanceCommandHandler(ResourceRegistry& resources) noexcept : resources_(resources) {}

        CreateInstanceSender createInstance(CreateMeshInstance::Request&& input) noexcept
        {
            return uploadInstance(std::move(input)) | stdexec::then(create_instance_response_);
        }

        CreateInstancesSender createInstances(CreateMeshInstances::Request&& input) noexcept
        {
            return uploadInstances(std::move(input)) | stdexec::then(create_instances_response_);
        }

        UpdateTransformsSender updateTransforms(UpdateTransforms::Request&& input) noexcept
        {
            return applyTransformUpdates(std::move(input)) | stdexec::then(update_transforms_response_);
        }

        UpdateInstancesSender updateInstances(UpdateMeshInstances::Request&& input) noexcept
        {
            return applyInstanceUpdates(std::move(input)) | stdexec::then(update_instances_response_);
        }

        ReleaseInstanceSender releaseInstance(ReleaseMeshInstance::Request&& input) noexcept
        {
            return releaseStoredInstance(std::move(input)) | stdexec::then(release_instance_response_);
        }

        using Bindings = std::tuple<
            CommandMethod<CreateMeshInstance, &RasterMeshInstanceCommandHandler::createInstance>,
            CommandMethod<CreateMeshInstances, &RasterMeshInstanceCommandHandler::createInstances>,
            CommandMethod<UpdateTransforms, &RasterMeshInstanceCommandHandler::updateTransforms>,
            CommandMethod<UpdateMeshInstances, &RasterMeshInstanceCommandHandler::updateInstances>,
            CommandMethod<ReleaseMeshInstance, &RasterMeshInstanceCommandHandler::releaseInstance>>;

    private:
        exec::task<RenderResult<std::vector<MeshInstanceId>>> uploadInstances(CreateMeshInstances::Request&&) noexcept;
        exec::task<RenderResult<MeshInstanceId>> uploadInstance(CreateMeshInstance::Request&&) noexcept;
        exec::task<RenderResult<TransformApplied>> applyTransformUpdates(UpdateTransforms::Request&&) noexcept;
        exec::task<RenderResult<TransformApplied>> applyInstanceUpdates(UpdateMeshInstances::Request&&) noexcept;
        exec::task<RenderResult<Ack>> releaseStoredInstance(ReleaseMeshInstance::Request&&) noexcept;

        ResourceRegistry& resources_;
    };

} // namespace lux::render::detail
