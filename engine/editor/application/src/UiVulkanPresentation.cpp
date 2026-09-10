#include <lux/engine/editor/application/UiVulkanPresentation.hpp>
#include <lux/engine/editor/application/detail/PresentationThreadStart.hpp>

#include <lux/engine/ui/UISession.hpp>

#include <lux/engine/function/render/client/RenderProgramSession.hpp>
#include <lux/engine/function/render/client/genops/ViewCameraOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MaterialOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/LightOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ForwardMeshOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ShadowMapOperation.ops.hpp>
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
#include <mutex>
#include <array>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include <cstdio>
#include <limits>

namespace lux::editor::application::detail
{
    namespace
    {
        inline constexpr render::TypeId kUiDrawDataAttachment = 3U;
        inline constexpr const char* kSubmitOperationName = "LuxUiSubmitDrawData";

        struct UiDrawDataSubmitPayload final
        {
            std::uint32_t attachment_index{};
            render::ViewCameraUpdatePayload camera{};
            render::RenderTargetId target{};
            lux::ui::TextureHandle texture{};
        };
        static_assert(std::is_trivially_copyable_v<UiDrawDataSubmitPayload>);
        struct UiFrameSnapshot final
        {
            lux::ui::UiFrameSnapshot draw;
            std::shared_ptr<const void> cpu_lease;
        };

        struct ViewStatistics final
        {
            std::atomic<std::uint64_t> frames{}, slots{}, created{}, retired{}, misses{};
            std::atomic<std::uint64_t> events{}, dropped{};
        };

        struct TextureCache final
        {
            struct Entry
            {
                render::RenderTargetId target{};
                std::uint64_t revision{};
                std::uint32_t slot{};
                VkImageView view{};
                VkDescriptorSet descriptor{};
            };
            lux::ui::detail::UiVulkanRenderer* renderer{};
            std::array<Entry, 48> entries{};
            std::shared_ptr<ViewStatistics> statistics;
            void clear() noexcept
            {
                for (auto& entry : entries)
                {
                    if (renderer && entry.descriptor)
                    {
                        renderer->removeTexture(entry.descriptor);
                        ++statistics->retired;
                    }
                    entry = {};
                }
            }
            static void retire(void* owner, std::span<const VkImageView> views) noexcept
            {
                auto& cache = *static_cast<TextureCache*>(owner);
                for (auto& entry : cache.entries)
                {
                    if (!entry.descriptor || std::find(views.begin(), views.end(), entry.view) == views.end())
                        continue;
                    if (cache.renderer)
                    {
                        cache.renderer->removeTexture(entry.descriptor);
                        ++cache.statistics->retired;
                    }
                    entry = {};
                }
            }
        };

        struct ServerState final
        {
            std::unique_ptr<lux::ui::detail::UiVulkanRenderer> renderer;
            lux::ui::UiFrameSnapshot* pending_snapshot{};
            render::TypeId submit_operation{render::kInvalidTypeId};
            lux::ui::detail::UiFontAtlasSnapshot font;
            lux::editor::workbench::SceneViewFrame view{};
            render::RenderTargetRegistry* targets{};
            render::FrameStamp stamp{};
            VkSampler sampler{};
            std::shared_ptr<TextureCache> textures{std::make_shared<TextureCache>()};
            std::shared_ptr<ViewStatistics> statistics;

            static VkDescriptorSet resolve(void* user, lux::ui::TextureHandle token) noexcept
            {
                auto& state = *static_cast<ServerState*>(user);
                const auto missing = [&]() { ++state.statistics->misses; return VkDescriptorSet{}; };
                if (token != state.view.texture || !token.valid() || !state.targets)
                    return missing();
                auto* target = state.targets->tryGet(state.view.target);
                if (!target || !target->pool || !target->pool->valid())
                    return missing();
                auto& pool = *target->pool;
                pool.setViewRetirementObserver(state.textures, &TextureCache::retire);
                const auto& views = pool.binding().slot(render::TargetSlot::SCENE_COLOR).views;
                const auto slot = state.stamp.slotIndex();
                // Allocation alone does not establish the graph's final sampled layout.
                if (!pool.recorded(slot))
                    return VkDescriptorSet{};
                if (slot >= views.size())
                    return missing();
                state.statistics->slots.fetch_or(std::uint64_t{1} << slot, std::memory_order_relaxed);
                for (const auto& entry : state.textures->entries)
                    if (entry.descriptor && entry.target == state.view.target &&
                        entry.revision == pool.backingRevision() && entry.slot == slot)
                        return entry.descriptor;
                for (auto& entry : state.textures->entries)
                {
                    if (entry.descriptor)
                        continue;
                    const auto descriptor = state.renderer->addTexture(state.sampler, views[slot],
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    if (!descriptor)
                        return missing();
                    entry = {state.view.target, pool.backingRevision(), slot, views[slot], descriptor};
                    ++state.statistics->created;
                    return descriptor;
                }
                return missing();
            }
        };

        using Server = render::GeneralRenderServer;
        using Dispatcher = Server::Dispatcher;
        using DispatchContext = Dispatcher::Ctx;

        void handleSubmitDrawData(DispatchContext& context, const UiDrawDataSubmitPayload& payload)
        {
            auto* state = static_cast<ServerState*>(render::serverExtensionOf(context.user_state));
            if (state == nullptr || payload.attachment_index >= context.program.attachments.size())
                return;
            auto& attachment = context.program.attachments[payload.attachment_index];
            if (attachment.type_id != kUiDrawDataAttachment ||
                attachment.object_size != sizeof(UiFrameSnapshot))
            {
                return;
            }
            state->pending_snapshot = &static_cast<UiFrameSnapshot*>(attachment.object)->draw;
            state->view.camera = payload.camera;
            state->view.target = payload.target;
            state->view.texture = payload.texture;
        }

        std::uint32_t registerUiOperations(void* dispatcher, render::TypeId* operations, std::uint32_t capacity)
        {
            if (dispatcher == nullptr || operations == nullptr || capacity == 0U)
                return 0U;
            auto& target = *static_cast<Dispatcher*>(dispatcher);
            operations[0] = target.allocateAndRegisterUnary<
                UiDrawDataSubmitPayload,
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
                const auto& description =
                    binding.layout->slots[static_cast<std::size_t>(render::TargetSlot::SCENE_COLOR)];
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
                    // Chain the transition after FrameDriver's acquire semaphore wait.
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
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
                state_.textures->clear();
                state_.textures->renderer = nullptr;
                state_.renderer.reset();
                if (state_.sampler)
                    vkDestroySampler(resourceContext().logicalDevice(), state_.sampler, nullptr);
                setExtension(nullptr, nullptr);
            }

            [[nodiscard]] render::Expected<void> initialize(
                render::ServerConfig config,
                lux::ui::detail::UiFontAtlasSnapshot font,
                render::FeatureCatalog& catalog,
                std::shared_ptr<ViewStatistics> statistics
            )
            {
                auto initialized = render::GeneralRenderServer::init(std::move(config));
                if (!initialized)
                    return initialized;
                state_.font = std::move(font);
                state_.statistics = std::move(statistics);
                state_.textures->statistics = state_.statistics;
                state_.targets = &targets();
                VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
                sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
                sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
                    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                const auto sampler_result =
                    vkCreateSampler(resourceContext().logicalDevice(), &sampler, nullptr, &state_.sampler);
                if (sampler_result != VK_SUCCESS)
                    return render::renderFailure<render::err::device::VulkanObjectCreationFailed>();
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
#if LUX_EDITOR_PACKED_CONTENT
                const std::array factories{
                    &render::kViewCameraFeatureFactory, &render::kMaterialFeatureFactory,
                    &render::kMeshStackFeatureFactory, &render::kLightFeatureFactory,
                    &render::kForwardMeshFeatureFactory, &render::kShadowMapFeatureFactory
                };
                for (const auto* feature : factories)
                {
                    const auto registered = addFeatureFactory(*feature);
                    if (!registered.error.ok() || registered.feature_type_id == 0)
                        return lux::cxx::unexpected(registered.error);
                    catalog.add(*feature, registered.feature_type_id, {registered.ops, registered.op_count});
                }
#endif
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
                state_.stamp = frame.rt.stamp;
                ++state_.statistics->frames;
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
                state_.textures->clear();
                state_.textures->renderer = nullptr;
                state_.renderer.reset();
                auto& resources = resourceContext();
                auto renderer = lux::ui::detail::UiVulkanRenderer::create(
                    lux::ui::detail::UiVulkanRendererCreateInfo{
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
                state_.textures->renderer = state_.renderer.get();
                state_.renderer->setTextureResolver(&ServerState::resolve, &state_);
                return {};
            }

            ServerState state_;
        };
    } // namespace

    struct UiVulkanPresentation::Impl final
    {
        struct UploadQueue final
        {
            std::mutex mutex;
            std::array<std::shared_ptr<render::detail::PreparedUpload>, 64> pending{};
            std::size_t head{}, size{}, bytes{};
            bool accepting{true};

            static render::UploadSubmitNoReplyResult submit(
                void* owner, std::shared_ptr<render::detail::PreparedUpload> packet
            ) noexcept
            {
                auto& queue = *static_cast<UploadQueue*>(owner);
                std::lock_guard lock{queue.mutex};
                if (!queue.accepting)
                    return lux::cxx::unexpected(render::ERenderUploadSubmitError::STOPPING);
                if (queue.size == queue.pending.size())
                    return lux::cxx::unexpected(render::ERenderUploadSubmitError::QUEUE_FULL);
                const auto bytes = packet->packet.accountedBytes();
                if (bytes > 16U * 1024U * 1024U - queue.bytes)
                    return lux::cxx::unexpected(render::ERenderUploadSubmitError::BYTE_BUDGET_EXHAUSTED);
                queue.bytes += bytes;
                queue.pending[(queue.head + queue.size++) % queue.pending.size()] = std::move(packet);
                return {};
            }
        };
        std::shared_ptr<render::RenderProgramChannel<>> frame_channel;
        std::shared_ptr<render::RenderControlChannel<>> control_channel;
        std::shared_ptr<render::RenderUploadChannel<>> upload_channel;
        std::shared_ptr<render::RenderChannelSync> sync;
        std::unique_ptr<render::RenderProgramSession> programs;
        std::unique_ptr<render::RenderControlSession> control;
        std::unique_ptr<render::RenderUploadSession> uploads;
        std::shared_ptr<UploadQueue> upload_queue;
        render::RenderUploadClient upload_client;
        render::FeatureCatalog catalog;
        std::size_t leases{};
        const std::thread::id owner{std::this_thread::get_id()};
        lux::editor::workbench::SceneViewFrame view{};
        render::ProgramMemoryHints program_memory;
        std::jthread server_thread;
        std::atomic<std::uint8_t> startup_state{};
        render::RenderError startup_error{};
        render::TypeId submit_operation{render::kInvalidTypeId};
        bool joined{};
        bool defer_frames{};
        std::shared_ptr<ViewStatistics> statistics{std::make_shared<ViewStatistics>()};
    };

    UiVulkanPresentation::UiVulkanPresentation(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    UiVulkanPresentation::CreateResult UiVulkanPresentation::create(
        window::LuxWindow& window,
        lux::ui::UISession& session,
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
            impl->programs->setErrorEventHandler([statistics = impl->statistics](const auto& batch) {
                statistics->dropped += batch.dropped;
            }, [statistics = impl->statistics](const render::RenderErrorEvent& event) {
                statistics->events += event.occurrences;
                const auto message = render::formatRenderError(render::renderErrorRegistry(), event.error);
                std::fprintf(stderr, "Render event: %s (count=%u)\n", message.c_str(), event.occurrences);
            });
            impl->control = std::make_unique<render::RenderControlSession>(impl->control_channel, impl->sync);
            impl->uploads = std::make_unique<render::RenderUploadSession>(impl->upload_channel, impl->sync);
            impl->upload_queue = std::make_shared<Impl::UploadQueue>();
            impl->upload_client = render::RenderUploadClient::bind(impl->upload_queue, &Impl::UploadQueue::submit);
            impl->program_memory = config.program_memory;
            auto font = lux::ui::detail::captureUiFontAtlas(session);
            if (!font)
                return lux::cxx::unexpected(UiVulkanPresentationFailure{
                    font.error() == lux::ui::EUiInitError::ALLOCATION_FAILURE
                        ? EUiVulkanPresentationError::ALLOCATION_FAILURE : EUiVulkanPresentationError::INVALID_CONFIG,
                    {}
                });
            std::vector<const char*> extensions;
            const auto required = window::LuxWindow::requiredVulkanInstanceExtensions();
            extensions.assign(required.begin(), required.end());
            auto* raw = impl.get();
            auto server_thread = startPresentationThread([
                raw,
                &window,
                font = std::move(*font),
                extensions = std::move(extensions),
                validation = config.enable_validation
            ]() mutable {
                return std::jthread([
                    raw,
                    &window,
                    font = std::move(font),
                    extensions = std::move(extensions),
                    validation
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
                    server_config.validation_message_sink = [](std::uint32_t severity, std::string_view message) {
                        std::fprintf(stderr, "Vulkan validation severity=%u: %.*s\n", severity,
                            static_cast<int>(message.size()), message.data());
                    };
                    auto initialized = server.initialize(
                        std::move(server_config), std::move(font), raw->catalog, raw->statistics);
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
            });
            if (!server_thread)
            {
                return lux::cxx::unexpected(UiVulkanPresentationFailure{
                    server_thread.error(),
                    {}
                });
            }
            impl->server_thread = std::move(*server_thread);
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
    UiVulkanPresentation::present(lux::ui::UISession& session) noexcept
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
            static_cast<void>(impl_->programs->pumpReplies());
            if (impl_->programs->hasPendingSubmit())
            {
                static_cast<void>(impl_->programs->retryPendingSubmit());
                return {};
            }
            if (impl_->defer_frames)
                return {};
            if (!impl_->programs->beginFrame(impl_->program_memory))
            {
                return lux::cxx::unexpected(UiVulkanPresentationFailure{
                    EUiVulkanPresentationError::FRAME_SUBMIT_FAILURE,
                    impl_->programs->terminalError()
                });
            }
            auto captured = session.captureFrame();
            if (!captured)
                return lux::cxx::unexpected(
                    UiVulkanPresentationFailure{EUiVulkanPresentationError::FRAME_CAPTURE_FAILURE});
            auto snapshot = std::move(*captured);
            const auto attachment = impl_->programs->builder().emplaceAttachment<UiFrameSnapshot>(
                kUiDrawDataAttachment,
                UiFrameSnapshot{std::move(snapshot), impl_->view.cpu_lease}
            );
            UiDrawDataSubmitPayload payload{};
            payload.attachment_index = attachment;
            payload.camera = impl_->view.camera;
            payload.target = impl_->view.target;
            payload.texture = impl_->view.texture;
            if (impl_->view.texture.valid())
            {
                render::ViewCameraProxy camera{*impl_->programs,
                    impl_->catalog.ops<render::ViewCameraOperationIds>("StandardViewCamera")};
                camera.update({&impl_->view.camera, 1});
            }
            impl_->programs->builder().push(
                render::opcodes::CommandOp,
                impl_->submit_operation,
                payload
            );
            static_cast<void>(impl_->programs->trySubmitFrame());
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
        {
            {
                std::lock_guard lock{impl_->upload_queue->mutex};
                impl_->upload_queue->accepting = false;
            }
            impl_->sync->requestStop();
        }
    }

    bool UiVulkanPresentation::join() noexcept
    {
        if (impl_ == nullptr || impl_->joined)
            return impl_ != nullptr;
        requestStop();
        if (impl_->server_thread.joinable())
            impl_->server_thread.join();
        // The server no longer borrows draw attachments. Release every queued CPU snapshot.
        impl_->view = {};
        impl_->programs.reset();
        impl_->frame_channel->requests.currentRead().clear_keep_capacity();
        while (impl_->frame_channel->requests.tryAcquireRead())
            impl_->frame_channel->requests.currentRead().clear_keep_capacity();
        if (auto* unsubmitted = impl_->frame_channel->requests.tryBeginWrite())
            unsubmitted->clear_keep_capacity();
        pump();
        impl_->joined = true;
        const auto stats = diagnostics();
        std::fprintf(stderr, "SV1 render frames=%llu slots=%llu descriptors=%llu/%llu misses=%llu "
            "events=%llu dropped=%llu leases=%zu\n",
            static_cast<unsigned long long>(stats.frames), static_cast<unsigned long long>(stats.slot_mask),
            static_cast<unsigned long long>(stats.descriptors_created),
            static_cast<unsigned long long>(stats.descriptors_retired),
            static_cast<unsigned long long>(stats.texture_misses), static_cast<unsigned long long>(stats.render_events),
            static_cast<unsigned long long>(stats.dropped_events), impl_->leases);
        return true;
    }

    bool UiVulkanPresentation::stopping() const noexcept
    {
        return impl_ == nullptr || impl_->sync == nullptr || impl_->sync->isStopping();
    }

    lux::cxx::expected<lux::scene::RenderRuntimeLease, lux::scene::RenderRuntimeFailure>
    UiVulkanPresentation::acquire() noexcept
    {
        if (std::this_thread::get_id() != impl_->owner || impl_->leases == std::numeric_limits<std::size_t>::max())
            return lux::cxx::unexpected(lux::scene::RenderRuntimeFailure{
                lux::scene::ERenderRuntimeError::ACTIVATION_FAILURE});
        if (stopping())
            return lux::cxx::unexpected(lux::scene::RenderRuntimeFailure{lux::scene::ERenderRuntimeError::STOPPING});
        ++impl_->leases;
        return makeLease();
    }

    void UiVulkanPresentation::release() noexcept { --impl_->leases; }
    render::RenderControlSession& UiVulkanPresentation::control() noexcept { return *impl_->control; }
    render::RenderProgramSession& UiVulkanPresentation::programs() noexcept { return *impl_->programs; }
    render::RenderUploadClient UiVulkanPresentation::upload() noexcept { return impl_->upload_client; }
    const render::FeatureCatalog& UiVulkanPresentation::features() const noexcept { return impl_->catalog; }
    bool UiVulkanPresentation::framePending() const noexcept
    {
        return impl_->programs && impl_->programs->hasPendingSubmit();
    }
    void UiVulkanPresentation::deferNewFrames(bool defer) noexcept { impl_->defer_frames = defer; }
    workbench::SceneViewDiagnostics UiVulkanPresentation::diagnostics() const noexcept
    {
        const auto& stats = *impl_->statistics;
        return {stats.frames.load(), stats.slots.load(), stats.created.load(), stats.retired.load(),
            stats.misses.load(), stats.events.load(), stats.dropped.load()};
    }
    void UiVulkanPresentation::setViewFrame(const lux::editor::workbench::SceneViewFrame& frame) noexcept
    {
        impl_->view = frame;
    }

    void UiVulkanPresentation::pump()
    {
        static_cast<void>(impl_->control->pumpReplies());
        if (impl_->programs)
            static_cast<void>(impl_->programs->pumpReplies());
        impl_->uploads->pumpReplies();
        auto& queue = *impl_->upload_queue;
        for (;;)
        {
            std::shared_ptr<render::detail::PreparedUpload> packet;
            std::size_t bytes{};
            {
                std::lock_guard lock{queue.mutex};
                if (queue.size == 0)
                    break;
                packet = queue.pending[queue.head];
                bytes = packet->packet.accountedBytes();
            }
            if (stopping())
            {
                static_cast<void>(packet->callback.settleFailure(
                    render::renderError<render::err::comm::ChannelStopping>()));
                std::lock_guard lock{queue.mutex};
                queue.pending[queue.head].reset();
                queue.head = (queue.head + 1) % queue.pending.size();
                --queue.size;
                queue.bytes -= bytes;
                continue;
            }
            render::ReplyDispatchCallback callback{
                [packet](render::ReplyPacketView reply, const render::ReplyRecord& record) {
                    packet->callback(reply, record);
                },
                [packet](render::RenderError error) { static_cast<void>(packet->callback.settleFailure(error)); }
            };
            const auto submitted = packet->expected_reply_type == render::kInvalidTypeId ?
                impl_->uploads->trySubmitPreparedNoReply(packet->packet) :
                impl_->uploads->trySubmitPrepared(packet->packet, packet->expected_reply_type, std::move(callback));
            if (!submitted)
                break;
            std::lock_guard lock{queue.mutex};
            queue.pending[queue.head].reset();
            queue.head = (queue.head + 1) % queue.pending.size();
            --queue.size;
            queue.bytes -= bytes;
        }
    }

    void UiVulkanPresentation::drainViewFrames()
    {
        try
        {
            if (stopping() || !impl_->programs)
                return;
            if (impl_->programs->hasPendingSubmit())
            {
                static_cast<void>(impl_->programs->retryPendingSubmit());
                return;
            }
            if (!impl_->programs->beginFrame(impl_->program_memory))
                return;
            const auto attachment = impl_->programs->builder().emplaceAttachment<UiFrameSnapshot>(
                kUiDrawDataAttachment, UiFrameSnapshot{});
            UiDrawDataSubmitPayload payload{};
            payload.attachment_index = attachment;
            impl_->programs->builder().push(render::opcodes::CommandOp, impl_->submit_operation, payload);
            static_cast<void>(impl_->programs->trySubmitFrame());
        }
        catch (const std::bad_alloc&)
        {
            requestStop();
            static_cast<void>(join());
        }
    }
} // namespace lux::editor::application::detail
