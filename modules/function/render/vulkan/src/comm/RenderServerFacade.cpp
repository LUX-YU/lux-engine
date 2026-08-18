#include <lux/engine/render/comm/server/RenderServerImpl.hpp>

#include <lux/engine/description/ShaderInfo.hpp>
#include <lux/engine/description/Texture.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/renderer/FrameDriver.hpp>
#include <lux/engine/render/renderer/Renderer.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/TextureResources.hpp>
#include <lux/engine/render/resources/descriptor/BindlessCombinedSet.hpp>
#include <lux/engine/render/resources/mesh/MeshResources.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/scene/View.hpp>
#include <lux/engine/render/targets/PresentContext.hpp>
#include <lux/engine/render/targets/SwapchainProvider.hpp>

#include <algorithm>
#include <mutex>
#include <string>
#include <utility>

namespace lux::render
{
    namespace detail
    {
        Expected<void> bindSwapchainInternal(
            GeneralRenderServer::Impl& impl,
            RenderSceneId scene_id,
            ViewHandle view,
            const RenderTargetLayout& layout,
            bool replace_existing
        )
        {
            using Entry = GeneralRenderServer::Impl::RenderTargetEntry;

            if (!impl.swapchainProvider())
                return renderFailure<err::internal::Unspecified>();

            auto* target = impl.surfaceTarget();
            if (target && !target->layers.empty() && !replace_existing)
                return renderFailure<err::internal::Unspecified>();

            auto* scene = impl.renderer_->getScene(scene_id);
            if (!scene || !scene->getView(view))
                return renderFailure<err::internal::Unspecified>();

            scene->compileGraphTemplate(layout);
            if (!target)
            {
                Entry entry{};
                entry.kind = Entry::EKind::Surface;
                entry.layout = layout;
                impl.targets_registry_.setSurfaceTarget(
                    impl.targets_registry_.insert(std::move(entry))
                );
                target = impl.targets_registry_.surfaceTarget();
            }

            target->layout = layout;
            target->layers.clear();
            target->layers.push_back(
                Entry::CompositeLayer::sceneView(scene_id, view)
            );

            const auto offscreen =
                impl.findOffscreenKeyByView(scene_id, view);
            if (offscreen.isValid())
            {
                impl.detachLayerAndReapIfEmpty(
                    offscreen,
                    scene_id,
                    view,
                    impl.current_stamp_.serial
                );
            }
            return {};
        }
    } // namespace detail

    RenderTargetRegistry& GeneralRenderServer::targets() noexcept
    {
        return impl_->targets();
    }

    const RenderTargetRegistry& GeneralRenderServer::targets() const noexcept
    {
        return impl_->targets();
    }

    Renderer& GeneralRenderServer::renderer() noexcept
    {
        return *impl_->renderer_;
    }

    ResourceContext& GeneralRenderServer::resourceContext() noexcept
    {
        return *impl_->res_ctx_;
    }

    DeviceContext* GeneralRenderServer::deviceContext() noexcept
    {
        return impl_->dev_ctx_.get();
    }

    Expected<DeviceCaps> GeneralRenderServer::deviceCaps() const noexcept
    {
        if (!impl_->dev_ctx_)
            return renderFailure<err::device::VulkanObjectCreationFailed>();
        return impl_->dev_ctx_->caps();
    }

    const lux::render::CapacityPlan&
    GeneralRenderServer::capacityPlan() const noexcept
    {
        static const lux::render::CapacityPlan empty{};
        return impl_->render_ctx_
            ? impl_->render_ctx_->capacityPlan()
            : empty;
    }

    SwapchainProvider* GeneralRenderServer::swapchainProvider() noexcept
    {
        return impl_->swapchainProvider();
    }

    const FrameStamp& GeneralRenderServer::currentStamp() const noexcept
    {
        return impl_->current_stamp_;
    }

    uint32_t GeneralRenderServer::framesInFlight() const noexcept
    {
        return impl_->frames_in_flight_;
    }

    uint64_t GeneralRenderServer::gpuCompletedSerial() const noexcept
    {
        return impl_->frame_driver_
            ? impl_->frame_driver_->gpuCompletedSerial()
            : impl_->current_stamp_.serial;
    }

    void GeneralRenderServer::setExtension(
        void* extension,
        PreDestroySceneCallback pre_destroy
    ) noexcept
    {
        impl_->extension_ = extension;
        impl_->pre_destroy_scene_cb_ = pre_destroy;
    }

    void* GeneralRenderServer::extension() const noexcept
    {
        return impl_->extension_;
    }

    void* serverExtensionOf(void* user_state) noexcept
    {
        return user_state
            ? static_cast<GeneralRenderServer::Impl*>(user_state)->extension_
            : nullptr;
    }

    void GeneralRenderServer::deferSurfaceRelease(
        RenderTargetId target,
        std::unique_ptr<PresentContext> context,
        std::function<void()> on_teardown
    )
    {
        Impl::PendingSurfaceRelease release{};
        release.target = target;
        release.retire_serial = impl_->frame_driver_
            ? impl_->frame_driver_->lastSubmittedSerial()
            : 0;
        release.ctx = std::move(context);
        release.on_teardown = std::move(on_teardown);
        impl_->pending_surface_releases_.push_back(std::move(release));
    }

    void GeneralRenderServer::flushPendingSurfaceReleases()
    {
        for (auto& release : impl_->pending_surface_releases_)
        {
            if (release.ctx)
            {
                auto closed = release.ctx->close();
                if (!closed)
                    renderFatal("pending PresentContext close failed during flush");
            }
            release.ctx.reset();
            if (release.on_teardown)
                release.on_teardown();
        }
        impl_->pending_surface_releases_.clear();
    }

    FeatureTypeRegisteredReply GeneralRenderServer::addFeatureFactory(
        const FeatureFactory& factory
    )
    {
        if (!impl_->renderer_)
        {
            return FeatureTypeRegisteredReply{
                .error =
                    renderError<err::device::VulkanObjectCreationFailed>()
            };
        }

        auto& registry = impl_->renderer_->featureTypeRegistry();
        FeatureTypeRecord record{};
        record.factory = factory;
        auto result = registry.add(std::move(record));

        FeatureTypeRegisteredReply reply{};
        if (!result)
        {
            reply.error = result.error();
            return reply;
        }

        reply.feature_type_id = result->type_id;
        reply.status = static_cast<std::uint32_t>(result->status);

        auto& stored = registry.at(result->type_id);
        if (result->status == EFeatureTypeRegisterStatus::Registered &&
            factory.register_ops_fn)
        {
            stored.op_count = factory.register_ops_fn(
                &impl_->dispatcher,
                stored.ops,
                FeatureTypeRegistry::kMaxOps
            );
            stored.op_count = std::min(
                stored.op_count,
                FeatureTypeRegistry::kMaxOps
            );
        }
        reply.op_count = stored.op_count;
        std::copy_n(stored.ops, stored.op_count, reply.ops);
        return reply;
    }

    std::vector<FeatureTypeRegisteredReply>
    GeneralRenderServer::addFeatureFactories(
        std::span<const FeatureFactory> factories
    )
    {
        std::vector<FeatureTypeRegisteredReply> results;
        results.reserve(factories.size());
        for (const auto& factory : factories)
            results.push_back(addFeatureFactory(factory));
        return results;
    }

    GeneralRenderServer::CreateSceneResult GeneralRenderServer::createScene(
        std::string_view name,
        std::span<const FeatureInitParam> features,
        lux::common::ETextureFormat lit_color_format
    )
    {
        RenderScene::Config config{};
        config.scene_name = std::string(name);
        config.pipeline.lit_color_format = lit_color_format;
        auto scene_result = impl_->renderer_->addScene(std::move(config));

        CreateSceneResult result;
        result.scene_id = scene_result.scene_id;
        auto* scene = impl_->renderer_->getScene(result.scene_id);
        result.features.reserve(features.size());
        result.feature_errors.reserve(features.size());

        for (const auto& parameter : features)
        {
            const Expected<FeatureHandle> installed = [&]()
                -> Expected<FeatureHandle>
            {
                if (!scene)
                {
                    return renderFailure<err::scene::NotFound>(
                        result.scene_id.index
                    );
                }

                const auto& feature =
                    impl_->renderer_->featureTypeRegistry()
                        .at(parameter.feature_type_id)
                        .factory;
                RenderScene::FeatureInstallScope scope(
                    *scene,
                    feature.descriptor
                );
                return feature.create_fn(
                    scene,
                    parameter.param,
                    parameter.param_size
                );
            }();

            result.features.push_back(
                installed ? *installed : FeatureHandle{}
            );
            result.feature_errors.push_back(
                installed ? RenderError{} : installed.error()
            );
        }
        return result;
    }

    Expected<ViewHandle> GeneralRenderServer::createView(
        const ViewInitParam& parameter
    )
    {
        auto* scene = impl_->renderer_->getScene(parameter.scene_id);
        if (!scene)
            return renderFailure<err::internal::Unspecified>();

        return scene->addView(ViewCreateInfo{
            .initial_extent = parameter.extent,
            .debug_name = parameter.name.data(),
        });
    }

    Expected<void> GeneralRenderServer::bindSwapchain(
        RenderSceneId scene_id,
        ViewHandle view,
        const RenderTargetLayout& layout
    )
    {
        return detail::bindSwapchainInternal(
            *impl_,
            scene_id,
            view,
            layout,
            false
        );
    }

    void GeneralRenderServer::unbindSwapchain()
    {
        if (auto* target = impl_->surfaceTarget())
            target->layers.clear();
    }

    bool GeneralRenderServer::hasSwapchainBinding() const noexcept
    {
        const auto* target = impl_->targets_registry_.surfaceTarget();
        return target && !target->layers.empty();
    }

    RenderTargetLayout GeneralRenderServer::swapchainLayout() const
    {
        auto* provider = impl_->swapchainProvider();
        return provider ? provider->layout() : RenderTargetLayout{};
    }

    Expected<RTextureHandle> GeneralRenderServer::createTexture2D(
        const lux::rdesc::Texture& texture,
        bool generate_mips
    )
    {
        auto& resources =
            impl_->render_ctx_->globalRegistry().must<TextureResources>();
        auto result = resources.submit(
            texture,
            nullptr,
            VK_FORMAT_UNDEFINED,
            generate_mips
        );
        if (!result)
            return lux::cxx::unexpected(result.error());
        return RTextureHandle{result->index, result->gen};
    }

    ShaderHandle GeneralRenderServer::compileShader(
        std::span<const std::byte> spirv,
        const lux::rdesc::ShaderInfo* info
    )
    {
        auto& resources =
            impl_->render_ctx_->globalRegistry().must<ShaderResources>();
        const lux::rdesc::ShaderInfo default_info{};
        return resources.add(spirv, info ? *info : default_info);
    }

    Expected<void> GeneralRenderServer::flushPendingGpuTransfers()
    {
        auto& resource_context = *impl_->res_ctx_;
        const VkDevice device = resource_context.logicalDevice();
        const VkCommandPool pool = resource_context.commandPool().handle();

        VkCommandBufferAllocateInfo allocation{};
        allocation.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocation.commandPool = pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;

        VkCommandBuffer command = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(device, &allocation, &command) != VK_SUCCESS)
            return renderFailure<err::internal::Unspecified>();

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(command, &begin);

        auto* mesh_resources =
            impl_->render_ctx_->globalRegistry().find<MeshResources>();
        auto& texture_resources =
            impl_->render_ctx_->globalRegistry().must<TextureResources>();
        texture_resources.bindlessSet2D().flushUploads(command, 0);
        texture_resources.bindlessSetCube().flushUploads(command, 0);
        vkEndCommandBuffer(command);

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        vkCreateFence(device, &fence_info, nullptr, &fence);

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        {
            const std::scoped_lock queue_lock(
                resource_context.deviceContext().graphicsQueueMutex());
            vkQueueSubmit(
                resource_context.graphicsQueue(),
                1,
                &submit,
                fence
            );
        }
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);

        if (mesh_resources)
            mesh_resources->retireFrameStagingBuffers(0);
        texture_resources.bindlessSet2D().retireDeferredStaging(0);
        texture_resources.bindlessSetCube().retireDeferredStaging(0);

        vkDestroyFence(device, fence, nullptr);
        vkFreeCommandBuffers(device, pool, 1, &command);
        return {};
    }
} // namespace lux::render
