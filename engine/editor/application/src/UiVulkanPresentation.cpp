#include <lux/engine/editor/application/UiVulkanPresentation.hpp>

#include <lux/engine/ui/UISession.hpp>

#include <lux/engine/function/render/client/RenderProgramSession.hpp>
#include <lux/engine/function/render/client/RenderProtocol.hpp>
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/renderer/FrameOrchestrator.hpp>
#include <lux/engine/render/renderer/RenderTargetRegistry.hpp>
#include <lux/engine/render/targets/RenderTargetBinding.hpp>
#include <lux/engine/render/targets/SwapchainProvider.hpp>
#include <lux/engine/window/LuxWindow.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace lux::editor::application::detail
{
    namespace
    {
        inline constexpr render::TypeId kUiDrawDataAttachment = 3U;
        inline constexpr const char* kSubmitOperationName = "LuxUiSubmitDrawData";

        struct ServerState final
        {
            std::unique_ptr<ui::detail::UiVulkanRenderer> renderer;
            ui::detail::UiDrawDataSnapshot* pending_snapshot{};
            render::TypeId submit_operation{render::kInvalidTypeId};
            ui::detail::UiFontAtlasSnapshot font;
        };

        using Server = render::GeneralRenderServer;
        using Dispatcher = Server::Dispatcher;
        using DispatchContext = Dispatcher::Ctx;

        void handleSubmitDrawData(DispatchContext& context, const render::SubmitImGuiDrawDataPayload& payload)
        {
            auto* state = static_cast<ServerState*>(render::serverExtensionOf(context.user_state));
            if (state == nullptr || payload.attachment_index >= context.program.attachments.size())
                return;
            auto& attachment = context.program.attachments[payload.attachment_index];
            if (attachment.type_id != kUiDrawDataAttachment ||
                attachment.object_size != sizeof(ui::detail::UiDrawDataSnapshot))
            {
                return;
            }
            state->pending_snapshot = static_cast<ui::detail::UiDrawDataSnapshot*>(attachment.object);
        }

        std::uint32_t registerUiOperations(void* dispatcher, render::TypeId* operations, std::uint32_t capacity)
        {
            if (dispatcher == nullptr || operations == nullptr || capacity == 0U)
                return 0U;
            auto& target = *static_cast<Dispatcher*>(dispatcher);
            operations[0] = target.allocateAndRegisterUnary<
                render::SubmitImGuiDrawDataPayload,
                &handleSubmitDrawData
            >(render::opcodes::CommandOp, kSubmitOperationName);
            return operations[0] == render::kInvalidTypeId ? 0U : 1U;
        }

        void unregisterUiOperations(void* dispatcher, const render::TypeId* operations, std::uint32_t count)
        {
            if (dispatcher == nullptr || operations == nullptr)
                return;
            auto& target = *static_cast<Dispatcher*>(dispatcher);
            for (std::uint32_t index = 0U; index < count; ++index)
                target.freeSlot(render::opcodes::CommandOp, operations[index]);
        }

        render::Expected<render::FeatureHandle> rejectUiFeatureCreation(void*, const void*, std::size_t)
        {
            return render::renderFailure<render::err::internal::Unspecified>();
        }

        void recordOverlay(
            VkCommandBuffer command,
            const render::RenderTargetBinding& binding,
            const render::LayerPhase& phase,
            ServerState& state
        )
        {
            const auto& slot = binding.slot(render::TargetSlot::SCENE_COLOR);
            if (command == VK_NULL_HANDLE || state.renderer == nullptr || slot.images.empty() || slot.views.empty())
                return;
            const VkImage image = slot.images.front();
            const VkImageView view = slot.views.front();
            if (image == VK_NULL_HANDLE || view == VK_NULL_HANDLE)
                return;

            VkImageLayout final_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            if (binding.layout != nullptr)
            {
                const auto& description = binding.layout->slots[static_cast<std::size_t>(render::TargetSlot::SCENE_COLOR)];
                if (description)
                    final_layout = render::toVkImageLayout(description->final_state);
            }

            const auto barrier = [&](VkImageLayout old_layout, VkImageLayout new_layout,
                                     VkPipelineStageFlags2 source_stage, VkAccessFlags2 source_access,
                                     VkPipelineStageFlags2 target_stage, VkAccessFlags2 target_access) {
                VkImageMemoryBarrier2 image_barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                image_barrier.srcStageMask = source_stage;
                image_barrier.srcAccessMask = source_access;
                image_barrier.dstStageMask = target_stage;
                image_barrier.dstAccessMask = target_access;
                image_barrier.oldLayout = old_layout;
                image_barrier.newLayout = new_layout;
                image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                image_barrier.image = image;
                image_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, 1U, 0U, 1U};
                VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.imageMemoryBarrierCount = 1U;
                dependency.pImageMemoryBarriers = &image_barrier;
                vkCmdPipelineBarrier2(command, &dependency);
            };

            if (phase.is_first)
            {
                barrier(
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                    VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
                );
            }
            else
            {
                barrier(
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
                );
            }

            VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            color.imageView = view;
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = phase.is_first ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color.clearValue.color = {{0.10F, 0.10F, 0.12F, 1.0F}};
            VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
            rendering.renderArea = {{0, 0}, binding.extent};
            rendering.layerCount = 1U;
            rendering.colorAttachmentCount = 1U;
            rendering.pColorAttachments = &color;
            vkCmdBeginRendering(command, &rendering);
            state.renderer->render(state.pending_snapshot, command);
            vkCmdEndRendering(command);

            if (phase.is_last && final_layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
            {
                barrier(
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    final_layout,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                    VK_ACCESS_2_NONE
                );
            }
        }

        void recordOverlayLayer(
            void* user,
            VkCommandBuffer command,
            const render::RenderTargetBinding& binding,
            const render::LayerPhase& phase
        )
        {
            auto* state = static_cast<ServerState*>(user);
            if (state != nullptr)
                recordOverlay(command, binding, phase, *state);
        }

        class MainWindowUiRenderServer final : public render::GeneralRenderServer
        {
        public:
            using render::GeneralRenderServer::GeneralRenderServer;

            ~MainWindowUiRenderServer() override
            {
                if (auto* provider = swapchainProvider())
                    provider->setRebuildCallback({});
                if (auto* device = deviceContext())
                    static_cast<void>(device->logicalDevice().waitIdle());
                state_.renderer.reset();
                setExtension(nullptr, nullptr);
            }

            [[nodiscard]] render::Expected<void> initialize(
                render::ServerConfig config,
                ui::detail::UiFontAtlasSnapshot font
            )
            {
                auto initialized = render::GeneralRenderServer::init(std::move(config));
                if (!initialized)
                    return initialized;
                state_.font = std::move(font);
                setExtension(&state_, nullptr);
                const render::FeatureFactory factory{
                    &rejectUiFeatureCreation,
                    &registerUiOperations,
                    &unregisterUiOperations,
                    "LuxMainWindowUi",
                    -1,
                    {}
                };
                const auto registration = addFeatureFactory(factory);
                if (!registration.error.ok() || registration.op_count != 1U)
                    return lux::cxx::unexpected(registration.error);
                state_.submit_operation = registration.ops[0];
                return {};
            }

            [[nodiscard]] render::Expected<void> attach(window::LuxWindow& window)
            {
                auto attached = render::GeneralRenderServer::attachToWindow(window);
                if (!attached)
                    return attached;
                auto rebuilt = rebuildRenderer();
                if (!rebuilt)
                    return rebuilt;
                swapchainProvider()->setRebuildCallback([this]() { return rebuildRenderer(); });
                return {};
            }

            [[nodiscard]] render::TypeId submitOperation() const noexcept { return state_.submit_operation; }

            bool tick() override
            {
                if (!drainTick())
                    return false;
                auto* surface = targets().surfaceTarget();
                if (surface != nullptr)
                {
                    surface->layers.clear();
                    surface->layers.push_back(render::RenderTargetEntry::CompositeLayer::customRecord(
                        &recordOverlayLayer,
                        &state_
                    ));
                }
                render::FrameTickState frame{};
                const auto start = beginRenderTick(frame);
                if (start == ETickStage::Failed)
                    return false;
                if (start == ETickStage::NoTarget || start == ETickStage::Skipped)
                {
                    state_.pending_snapshot = nullptr;
                    return stepPendingSurfaceReleases();
                }
                renderRenderTick(frame);
                state_.pending_snapshot = nullptr;
                return endRenderTick(frame);
            }

        private:
            [[nodiscard]] render::Expected<void> rebuildRenderer()
            {
                auto* provider = swapchainProvider();
                if (provider == nullptr || provider->imageCount() == 0U)
                    return {};
                state_.renderer.reset();
                auto& resources = resourceContext();
                auto renderer = ui::detail::UiVulkanRenderer::create(
                    ui::detail::UiVulkanRendererCreateInfo{
                        resources.instanceContext().instance(),
                        resources.physicalDevice(),
                        resources.logicalDevice(),
                        resources.graphicsQueueFamilyIndex(),
                        resources.graphicsQueue(),
                        provider->format(),
                        provider->imageCount(),
                        resources.instanceContext().allocator()
                    },
                    state_.font
                );
                if (!renderer)
                    return render::renderFailure<render::err::device::VulkanObjectCreationFailed>();
                state_.renderer = std::move(*renderer);
                return {};
            }

            ServerState state_;
        };
    } // namespace

    struct UiVulkanPresentation::Impl final
    {
        std::shared_ptr<render::RenderProgramChannel<>> frame_channel;
        std::shared_ptr<render::RenderControlChannel<>> control_channel;
        std::shared_ptr<render::RenderUploadChannel<>> upload_channel;
        std::shared_ptr<render::RenderChannelSync> sync;
        std::unique_ptr<render::RenderProgramSession> programs;
        render::ProgramMemoryHints program_memory;
        std::jthread server_thread;
        std::atomic<std::uint8_t> startup_state{};
        render::RenderError startup_error{};
        render::TypeId submit_operation{render::kInvalidTypeId};
        bool joined{};
    };

    UiVulkanPresentation::UiVulkanPresentation(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    UiVulkanPresentation::CreateResult UiVulkanPresentation::create(
        window::LuxWindow& window,
        ui::UISession& session,
        UiVulkanPresentationConfig config
    ) noexcept
    {
        const bool invalid_capacity = config.frame_capacity == 0U || config.control_capacity == 0U ||
            config.upload_capacity == 0U || config.upload_byte_capacity == 0U;
        if (invalid_capacity || !window.isInitialized())
        {
            return lux::cxx::unexpected(UiVulkanPresentationFailure{
                EUiVulkanPresentationError::INVALID_CONFIG,
                {}
            });
        }
        try
        {
            auto impl = std::make_unique<Impl>();
            impl->frame_channel = render::RenderProgramChannel<>::create(config.frame_capacity);
            impl->control_channel = render::RenderControlChannel<>::create(config.control_capacity);
            impl->upload_channel = render::RenderUploadChannel<>::create(
                config.upload_capacity,
                config.upload_byte_capacity
            );
            impl->sync = std::make_shared<render::RenderChannelSync>();
            impl->programs = std::make_unique<render::RenderProgramSession>(impl->frame_channel, impl->sync);
            impl->program_memory = config.program_memory;
            auto font = ui::detail::captureUiFontAtlas(session);
            std::vector<const char*> extensions;
            const auto required = window::LuxWindow::requiredVulkanInstanceExtensions();
            extensions.assign(required.begin(), required.end());
            auto* raw = impl.get();
            impl->server_thread = std::jthread([
                raw,
                &window,
                font = std::move(font),
                extensions = std::move(extensions),
                validation = config.enable_validation
            ]() mutable {
                MainWindowUiRenderServer server(
                    raw->frame_channel,
                    raw->control_channel,
                    raw->upload_channel,
                    raw->sync
                );
                render::ServerConfig server_config;
                server_config.instance_extensions = std::move(extensions);
                server_config.enable_validation = validation;
                auto initialized = server.initialize(std::move(server_config), std::move(font));
                if (initialized)
                    initialized = server.attach(window);
                if (!initialized)
                {
                    raw->startup_error = initialized.error();
                    raw->startup_state.store(2U, std::memory_order_release);
                    raw->startup_state.notify_all();
                    raw->sync->requestStop();
                    return;
                }
                raw->submit_operation = server.submitOperation();
                raw->startup_state.store(1U, std::memory_order_release);
                raw->startup_state.notify_all();
                while (server.tick())
                {
                }
                raw->sync->requestStop();
            });
            while (impl->startup_state.load(std::memory_order_acquire) == 0U)
                impl->startup_state.wait(0U, std::memory_order_acquire);
            if (impl->startup_state.load(std::memory_order_acquire) != 1U)
            {
                impl->server_thread.join();
                return lux::cxx::unexpected(UiVulkanPresentationFailure{
                    EUiVulkanPresentationError::RENDER_START_FAILURE,
                    impl->startup_error
                });
            }
            return std::unique_ptr<UiVulkanPresentation>{new UiVulkanPresentation(std::move(impl))};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(UiVulkanPresentationFailure{
                EUiVulkanPresentationError::ALLOCATION_FAILURE,
                {}
            });
        }
    }

    UiVulkanPresentation::~UiVulkanPresentation() noexcept
    {
        requestStop();
        static_cast<void>(join());
    }

    lux::cxx::expected<void, UiVulkanPresentationFailure>
    UiVulkanPresentation::present(ui::UISession& session) noexcept
    {
        if (stopping())
        {
            return lux::cxx::unexpected(UiVulkanPresentationFailure{
                EUiVulkanPresentationError::STOPPING,
                impl_->sync->terminalError()
            });
        }
        try
        {
            if (!impl_->programs->beginFrame(impl_->program_memory))
            {
                return lux::cxx::unexpected(UiVulkanPresentationFailure{
                    EUiVulkanPresentationError::FRAME_SUBMIT_FAILURE,
                    impl_->programs->terminalError()
                });
            }
            auto snapshot = ui::detail::captureUiDrawData(session);
            const auto attachment = impl_->programs->builder().emplaceAttachment<ui::detail::UiDrawDataSnapshot>(
                kUiDrawDataAttachment,
                std::move(snapshot)
            );
            render::SubmitImGuiDrawDataPayload payload{};
            payload.attachment_index = attachment;
            impl_->programs->builder().push(
                render::opcodes::CommandOp,
                impl_->submit_operation,
                payload
            );
            while (!impl_->programs->trySubmitFrame())
            {
                if (stopping())
                {
                    return lux::cxx::unexpected(UiVulkanPresentationFailure{
                        EUiVulkanPresentationError::STOPPING,
                        impl_->programs->terminalError()
                    });
                }
                static_cast<void>(impl_->programs->pumpReplies());
                const auto observed = impl_->programs->observeProgress();
                static_cast<void>(impl_->programs->waitForProgressUntil(
                    observed,
                    std::chrono::steady_clock::now() + std::chrono::milliseconds{2}
                ));
            }
            static_cast<void>(impl_->programs->pumpReplies());
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(UiVulkanPresentationFailure{
                EUiVulkanPresentationError::FRAME_CAPTURE_FAILURE,
                {}
            });
        }
    }

    void UiVulkanPresentation::requestStop() noexcept
    {
        if (impl_ != nullptr && impl_->sync != nullptr)
            impl_->sync->requestStop();
    }

    bool UiVulkanPresentation::join() noexcept
    {
        if (impl_ == nullptr || impl_->joined)
            return impl_ != nullptr;
        requestStop();
        if (impl_->server_thread.joinable())
            impl_->server_thread.join();
        impl_->joined = true;
        return true;
    }

    bool UiVulkanPresentation::stopping() const noexcept
    {
        return impl_ == nullptr || impl_->sync == nullptr || impl_->sync->isStopping();
    }
} // namespace lux::editor::application::detail
