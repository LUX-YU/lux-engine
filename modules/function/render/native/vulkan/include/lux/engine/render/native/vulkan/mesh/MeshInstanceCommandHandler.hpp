#pragma once

#include <exec/task.hpp>
#include <lux/engine/render/commands/CommandMethod.hpp>

#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/resource/MeshInstance.hpp>

namespace lux::render::vulkan
{
    class DeviceSession;
    class MeshInstanceStorage;

    class MeshInstanceCommandHandler final
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

        static constexpr std::string_view name = "lux.render.vulkan.MeshInstanceCommandHandler";

        explicit MeshInstanceCommandHandler(DeviceSession& value) noexcept : device_(value) {}

        CreateInstanceSender createInstance(CreateMeshInstance::Request&& input) noexcept
        {
            return createInstanceSource(std::move(input)) | stdexec::then(create_instance_response_);
        }

        CreateInstancesSender createInstances(CreateMeshInstances::Request&& input) noexcept
        {
            return createInstancesSource(std::move(input)) | stdexec::then(create_instances_response_);
        }

        UpdateTransformsSender updateTransforms(UpdateTransforms::Request&& input) noexcept
        {
            return updateTransformsSource(std::move(input)) | stdexec::then(update_transforms_response_);
        }

        UpdateInstancesSender updateInstances(UpdateMeshInstances::Request&& input) noexcept
        {
            return updateInstancesSource(std::move(input)) | stdexec::then(update_instances_response_);
        }

        ReleaseInstanceSender releaseInstance(ReleaseMeshInstance::Request&& input) noexcept
        {
            return releaseInstanceSource(std::move(input)) | stdexec::then(release_instance_response_);
        }

        using Bindings = std::tuple<
            CommandMethod<CreateMeshInstance, &MeshInstanceCommandHandler::createInstance>,
            CommandMethod<CreateMeshInstances, &MeshInstanceCommandHandler::createInstances>,
            CommandMethod<UpdateTransforms, &MeshInstanceCommandHandler::updateTransforms>,
            CommandMethod<UpdateMeshInstances, &MeshInstanceCommandHandler::updateInstances>,
            CommandMethod<ReleaseMeshInstance, &MeshInstanceCommandHandler::releaseInstance>>;

    private:
        exec::task<RenderResult<std::vector<MeshInstanceId>>> createInstancesSource(CreateMeshInstances::
                                                                                        Request&&) noexcept;
        exec::task<RenderResult<MeshInstanceId>> createInstanceSource(CreateMeshInstance::Request&&) noexcept;
        exec::task<RenderResult<TransformApplied>> updateTransformsSource(UpdateTransforms::Request&&) noexcept;
        exec::task<RenderResult<TransformApplied>> updateInstancesSource(UpdateMeshInstances::Request&&) noexcept;
        exec::task<RenderResult<Ack>> releaseInstanceSource(ReleaseMeshInstance::Request&&) noexcept;

        DeviceSession& device_;
    };

} // namespace lux::render::vulkan

namespace lux::render
{
    template <class Declaration> struct StorageProvider;

    template <> struct StorageProvider<render::MeshInstanceStorage> final
    {
        using Storage = vulkan::MeshInstanceStorage;
        using Handler = vulkan::MeshInstanceCommandHandler;

        [[nodiscard]] static std::unique_ptr<Handler> createHandler(vulkan::DeviceSession& device) noexcept
        {
            return std::make_unique<Handler>(device);
        }

        [[nodiscard]] static StorageFactory factory() noexcept;
        [[nodiscard]] static CppType implementationType() noexcept;
    };
} // namespace lux::render
