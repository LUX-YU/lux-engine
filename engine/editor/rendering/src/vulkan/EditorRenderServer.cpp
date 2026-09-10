#include <lux/engine/editor/rendering/detail/RendererThread.hpp>
#include <lux/engine/editor/rendering/detail/RenderFrameQueue.hpp>
#include <lux/engine/editor/rendering/detail/ViewImageLifetime.hpp>
#include <lux/engine/ui/detail/UiVulkanBackend.hpp>
#include <system_error>
#if defined(LUX_EDITOR_RENDERER_TEST_DIAGNOSTICS)
#include <lux/engine/editor/rendering/detail/RendererTestAccess.hpp>
#include <lux/engine/render/renderer/Renderer.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/scene/View.hpp>
#include <lux/engine/render/graph/RGRecorder.hpp>
#endif

#include <lux/engine/ui/UISession.hpp>

#include <lux/engine/function/render/client/RenderProgramSession.hpp>
#include <lux/engine/function/render/client/genops/ViewCameraOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MaterialOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/LightOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ForwardMeshOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ShadowMapOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshShadowOperation.ops.hpp>
#include <lux/engine/render/comm/server/RenderServer.hpp>
#if defined(LUX_EDITOR_RENDERER_TEST_DIAGNOSTICS)
#include <lux/engine/render/comm/server/FeatureOpRegistrar.hpp>
#endif
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

#if defined(LUX_EDITOR_RENDERER_TEST_DIAGNOSTICS)
extern "C" __declspec(dllimport) void
lux_er1_render_memory_statistics(VmaAllocator, std::uint64_t *, std::uint64_t *) noexcept;
#endif

#if defined(LUX_EDITOR_RENDERER_TEST_DIAGNOSTICS)
extern "C" __declspec(dllimport) void lux_er1_handle_destroy_material(
    lux::render::GeneralRenderServer::Dispatcher::Ctx &, const lux::render::DestroyMaterialPayload &);
extern "C" __declspec(dllimport) void lux_er1_handle_upload_material(
    lux::render::GeneralRenderServer::Dispatcher::Ctx &, const lux::render::UploadGraphMaterialPayload &);
extern "C" __declspec(dllimport) void lux_er1_handle_modify_material(
    lux::render::GeneralRenderServer::Dispatcher::Ctx &, const lux::render::ModifyGraphMaterialPayload &);
#endif

namespace lux::editor::rendering::detail
{
    namespace
    {
        inline constexpr const char *kSubmitOperationName = "LuxEditorSubmitFrame";
        struct ViewTextureTable final
        {
            struct Entry
            {
                render::RenderTargetId target{};
                std::uint64_t revision{};
                std::uint32_t slot{};
                VkImageView view{};
                VkDescriptorSet descriptor{};
            };
            lux::ui::detail::UiVulkanRenderer *renderer{};
            std::array<Entry, 48> entries{};
            std::shared_ptr<RenderStatistics> statistics;
            void clear() noexcept
            {
                for (auto &entry : entries)
                {
                    if (renderer && entry.descriptor)
                    {
                        renderer->removeTexture(entry.descriptor);
                        ++statistics->retired;
                    }
                    entry = {};
                }
            }
            static void retire(void *owner, std::span<const VkImageView> views) noexcept
            {
                auto &cache = *static_cast<ViewTextureTable *>(owner);
                for (auto &entry : cache.entries)
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
            FrameDrawData *pending_snapshot{};
            render::TypeId submit_operation{render::kInvalidTypeId};
            lux::ui::detail::UiFontAtlasSnapshot font;
            render::RenderTargetRegistry *targets{};
            render::FrameStamp stamp{};
            VkSampler sampler{};
            std::shared_ptr<ViewTextureTable> textures{std::make_shared<ViewTextureTable>()};
            std::shared_ptr<RenderStatistics> statistics;

            static VkDescriptorSet resolve(void *user, lux::ui::TextureHandle token) noexcept
            {
                auto &state = *static_cast<ServerState *>(user);
                const auto missing = [&]() {
                    ++state.statistics->misses;
                    return VkDescriptorSet{};
                };
                if (!state.pending_snapshot || !state.targets || !token.valid())
                    return missing();
                const auto found =
                    std::find_if(state.pending_snapshot->images.begin(), state.pending_snapshot->images.end(),
                                 [&](const auto &image) { return image.texture == token; });
                if (found == state.pending_snapshot->images.end())
                    return missing();
                const auto *record = ViewImageAccess::record(*found);
                if (!record)
                    return missing();
                auto &version = *record->version;
                auto *target = state.targets->tryGet(version.target);
                if (!target || !target->pool || !target->pool->valid())
                    return missing();
                auto &pool = *target->pool;
                auto revision = version.backing_revision.load(std::memory_order_acquire);
                if (revision == 0)
                {
                    version.backing_revision.store(pool.backingRevision(), std::memory_order_release);
                    revision = pool.backingRevision();
                }
                if (revision != pool.backingRevision())
                    return missing();
                pool.setViewRetirementObserver(state.textures, &ViewTextureTable::retire);
                const auto &views = pool.binding().slot(render::TargetSlot::SCENE_COLOR).views;
                const auto slot = state.stamp.slotIndex();
                // Allocation alone does not establish the graph's final sampled layout.
                if (!pool.recorded(slot))
                    return VkDescriptorSet{};
                if (slot >= views.size())
                    return missing();
                state.statistics->slots.fetch_or(std::uint64_t{1} << slot, std::memory_order_relaxed);
                for (const auto &entry : state.textures->entries)
                    if (entry.descriptor && entry.target == version.target &&
                        entry.revision == pool.backingRevision() && entry.slot == slot)
                        return entry.descriptor;
                for (auto &entry : state.textures->entries)
                {
                    if (entry.descriptor)
                        continue;
                    const auto descriptor = state.renderer->addTexture(state.sampler, views[slot],
                                                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    if (!descriptor)
                        return missing();
                    entry = {version.target, pool.backingRevision(), slot, views[slot], descriptor};
                    ++state.statistics->created;
                    return descriptor;
                }
                return missing();
            }
        };

        using Server = render::GeneralRenderServer;
        using Dispatcher = Server::Dispatcher;
        using DispatchContext = Dispatcher::Ctx;

#if defined(LUX_EDITOR_RENDERER_TEST_DIAGNOSTICS)
        void rejectSelectedMaterialUpload(DispatchContext &context, const render::UploadGraphMaterialPayload &payload)
        {
            auto &state = *static_cast<ServerState *>(render::serverExtensionOf(context.user_state));
            auto &stats = *state.statistics;
            const bool armed = stats.reject_material.load(std::memory_order_acquire);
            if (armed && payload.graph_forward_shader == stats.rejected_forward &&
                payload.graph_gbuffer_shader == stats.rejected_gbuffer)
            {
                // Explicit one-shot service fault, delivered by the real typed reply lane. This does
                // not claim that Vulkan or MaterialResources naturally exhausted an allocation.
                ++stats.material_rejections;
                stats.reject_material.store(false, std::memory_order_release);
                render::replyToCurrent<render::UploadGraphMaterialPayload>(
                    context, render::MaterialUploadedReply{{}, 1}
                );
                return;
            }
            lux_er1_handle_upload_material(context, payload);
        }
        using DiagnosticMaterialOps = render::FeatureOpRegistrar<
            render::ServerOp<render::DestroyMaterialOp, &lux_er1_handle_destroy_material>,
            render::ServerOp<render::UploadGraphMaterialOp, &rejectSelectedMaterialUpload>,
            render::ServerOp<render::ModifyGraphMaterialOp, &lux_er1_handle_modify_material>>;
#endif

        void handleSubmitDrawData(DispatchContext &context, const SubmitDrawPayload &payload)
        {
            auto *state = static_cast<ServerState *>(render::serverExtensionOf(context.user_state));
            if (state == nullptr || payload.attachment >= context.program.attachments.size())
                return;
            auto &attachment = context.program.attachments[payload.attachment];
            if (attachment.type_id != kUiDrawAttachment || attachment.object_size != sizeof(FrameDrawData))
            {
                return;
            }
            state->pending_snapshot = static_cast<FrameDrawData *>(attachment.object);
        }

        std::uint32_t registerUiOperations(void *dispatcher, render::TypeId *operations, std::uint32_t capacity)
        {
            if (dispatcher == nullptr || operations == nullptr || capacity == 0U)
                return 0U;
            auto &target = *static_cast<Dispatcher *>(dispatcher);
            operations[0] = target.allocateAndRegisterUnary<SubmitDrawPayload, &handleSubmitDrawData>(
                render::opcodes::CommandOp, kSubmitOperationName);
            return operations[0] == render::kInvalidTypeId ? 0U : 1U;
        }

        void unregisterUiOperations(void *dispatcher, const render::TypeId *operations, std::uint32_t count)
        {
            if (dispatcher == nullptr || operations == nullptr)
                return;
            auto &target = *static_cast<Dispatcher *>(dispatcher);
            for (std::uint32_t index = 0U; index < count; ++index)
                target.freeSlot(render::opcodes::CommandOp, operations[index]);
        }

        render::Expected<render::FeatureHandle> rejectUiFeatureCreation(void *, const void *, std::size_t)
        {
            return render::renderFailure<render::err::internal::Unspecified>();
        }

        void recordOverlay(VkCommandBuffer command, const render::RenderTargetBinding &binding,
                           const render::LayerPhase &phase, ServerState &state)
        {
            const auto &slot = binding.slot(render::TargetSlot::SCENE_COLOR);
            if (command == VK_NULL_HANDLE || state.renderer == nullptr || slot.images.empty() || slot.views.empty())
                return;
            const VkImage image = slot.images.front();
            const VkImageView view = slot.views.front();
            if (image == VK_NULL_HANDLE || view == VK_NULL_HANDLE)
                return;

            VkImageLayout final_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            if (binding.layout != nullptr)
            {
                const auto &description =
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
                barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        // Chain the transition after FrameDriver's acquire semaphore wait.
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            }
            else
            {
                barrier(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
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
            state.renderer->render(state.pending_snapshot ? &state.pending_snapshot->snapshot : nullptr, command);
            vkCmdEndRendering(command);

            if (phase.is_last && final_layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
            {
                barrier(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, final_layout,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, VK_ACCESS_2_NONE);
            }
        }

        void recordOverlayLayer(void *user, VkCommandBuffer command, const render::RenderTargetBinding &binding,
                                const render::LayerPhase &phase)
        {
            auto *state = static_cast<ServerState *>(user);
            if (state != nullptr)
                recordOverlay(command, binding, phase, *state);
        }

        class EditorRenderServer final : public render::GeneralRenderServer
        {
        public:
            using render::GeneralRenderServer::GeneralRenderServer;

            ~EditorRenderServer() override
            {
                if (auto *provider = swapchainProvider())
                    provider->setRebuildCallback({});
                if (auto *device = deviceContext(); device && device->logicalDevice())
                    static_cast<void>(device->logicalDevice().waitIdle());
                state_.textures->clear();
                state_.textures->renderer = nullptr;
                state_.renderer.reset();
                if (state_.sampler)
                    vkDestroySampler(resourceContext().logicalDevice(), state_.sampler, nullptr);
                setExtension(nullptr, nullptr);
            }

            [[nodiscard]] render::Expected<void> initialize(render::ServerConfig config,
                                                            lux::ui::detail::UiFontAtlasSnapshot font,
                                                            render::FeatureCatalog &catalog,
                                                            std::shared_ptr<RenderStatistics> statistics,
                                                            std::shared_ptr<render::RenderChannelSync> sync,
                                                            std::atomic<std::uint64_t> &failed_packet)
            {
                sync_ = std::move(sync);
                failed_packet_ = &failed_packet;
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
                const render::FeatureFactory factory{&rejectUiFeatureCreation,
                                                     &registerUiOperations,
                                                     &unregisterUiOperations,
                                                     "LuxMainWindowUi",
                                                     -1,
                                                     {}};
                const auto registration = addFeatureFactory(factory);
                if (!registration.error.ok() || registration.op_count != 1U)
                    return lux::cxx::unexpected(registration.error);
                state_.submit_operation = registration.ops[0];
#if LUX_EDITOR_PACKED_CONTENT
                const std::array factories{&render::kViewCameraFeatureFactory,  &render::kMaterialFeatureFactory,
                                           &render::kMeshStackFeatureFactory,   &render::kLightFeatureFactory,
                                           &render::kForwardMeshFeatureFactory, &render::kShadowMapFeatureFactory,
                                           &render::kMeshShadowFeatureFactory};
                for (const auto *feature : factories)
                {
                    auto installed = *feature;
#if defined(LUX_EDITOR_RENDERER_TEST_DIAGNOSTICS)
                    if (feature == &render::kMaterialFeatureFactory)
                        installed.register_ops_fn = &DiagnosticMaterialOps::registerAll;
#endif
                    const auto registered = addFeatureFactory(installed);
                    if (!registered.error.ok() || registered.feature_type_id == 0)
                        return lux::cxx::unexpected(registered.error);
                    catalog.add(*feature, registered.feature_type_id, {registered.ops, registered.op_count});
                }
#endif
                return {};
            }

            [[nodiscard]] render::Expected<void> attach(window::LuxWindow &window)
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

            [[nodiscard]] render::TypeId submitOperation() const noexcept
            {
                return state_.submit_operation;
            }

            bool tick() override
            {
                if (!drainTick())
                    return false;
#if defined(LUX_EDITOR_RENDERER_TEST_DIAGNOSTICS)
                if (state_.pending_snapshot && !state_.pending_snapshot->record_failure.ok())
                {
                    // Actual consumer has accepted this packet. Fail at record preparation, then use
                    // the ordinary terminal cleanup path, including the real device's completion wait.
                    failed_packet_->store(state_.pending_snapshot->packet_sequence, std::memory_order_release);
                    sync_->publishTerminalError(state_.pending_snapshot->record_failure);
                    return false;
                }
#endif
                auto *surface = targets().surfaceTarget();
                if (surface != nullptr)
                {
                    surface->layers.clear();
                    surface->layers.push_back(
                        render::RenderTargetEntry::CompositeLayer::customRecord(&recordOverlayLayer, &state_));
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
                state_.statistics->completed.store(gpuCompletedSerial(), std::memory_order_release);
                state_.stamp = frame.rt.stamp;
                ++state_.statistics->frames;
                renderRenderTick(frame);
#if defined(LUX_EDITOR_RENDERER_TEST_DIAGNOSTICS)
                if (state_.statistics->observe_memory.load(std::memory_order_acquire))
                {
                    auto trace = state_.statistics->memory_trace.load(std::memory_order_relaxed);
                    ++trace.samples;
                    lux_er1_render_memory_statistics(
                        deviceContext()->vmaAllocator(), &trace.last_bytes, &trace.last_allocations
                    );
                    trace.peak_bytes = std::max(trace.peak_bytes, trace.last_bytes);
                    trace.peak_allocations = std::max(trace.peak_allocations, trace.last_allocations);
                    state_.statistics->memory_trace.store(trace, std::memory_order_release);
                }
                if (state_.statistics->observe_shared.load(std::memory_order_acquire))
                    observeSharedImports(frame.rt.stamp.serial);
#endif
                const bool ended = endRenderTick(frame);
                if (ended && frame.rt.primary_cmd && state_.pending_snapshot)
                    for (const auto &image : state_.pending_snapshot->images)
                    {
                        const auto *record = ViewImageAccess::record(image);
                        record->version->last_recording.store(frame.rt.stamp.serial, std::memory_order_release);
                        record->version->last_submission.store(frame.rt.stamp.serial, std::memory_order_release);
                        record->submitted.store(frame.rt.stamp.serial, std::memory_order_release);
                    }
                state_.statistics->completed.store(gpuCompletedSerial(), std::memory_order_release);
                state_.pending_snapshot = nullptr;
                return ended;
            }

        private:
#if defined(LUX_EDITOR_RENDERER_TEST_DIAGNOSTICS)
            void observeSharedImports(std::uint64_t serial)
            {
                auto &stats = *state_.statistics;
                renderer().forEachScene([&](render::RenderScene &scene) {
                    const auto &state = scene.graphState();
                    if (!state.valid || !state.graph || !state.graph->barrier_program)
                        return;
                    const auto &graph = *state.graph;
                    for (std::uint32_t ri = 0; ri != graph.original_graph.resources.size(); ++ri)
                    {
                        const auto &resource = graph.original_graph.resources[ri];
                        if (!resource.import_info || resource.import_info->slot || !resource.import_info->image_getter)
                            continue;
                        std::uintptr_t first{};
                        unsigned matches{};
                        scene.forEachActiveView([&](const render::View &view) {
                            if (!view.resource_state)
                                return;
                            const auto *physical = view.resource_state->physical_resources.tryGet(ri);
                            if (!physical || physical->physical_handles.empty())
                                return;
                            const auto handle = physical->getHandle(0);
                            if (!handle)
                                return;
                            if (!first)
                                first = handle;
                            if (first != handle)
                                return;
                            ++matches;
                            const auto &skips = view.resource_state->record_ctx.cond_skip_scratch;
                            for (const auto pass_index : graph.execution_order)
                            {
                                if (pass_index >= skips.size() || skips[pass_index])
                                    continue;
                                const auto &pass = graph.compiled_passes[pass_index];
                                const auto &reads = pass.resources.read_images;
                                const auto &writes = pass.resources.write_images;
                                if (std::find(reads.begin(), reads.end(), ri) != reads.end())
                                    ++stats.shared_reads;
                                if (std::find(writes.begin(), writes.end(), ri) != writes.end())
                                    ++stats.shared_writes;
                            }
                        });
                        if (matches < 2)
                            continue;
                        stats.shared_pairs += matches - 1;
                        stats.shared_image.store(first);
                        if (stats.shared_first_serial.load() == 0)
                            stats.shared_first_serial.store(serial);
                        for (const auto &group : graph.barrier_program->subsequent_view_barriers)
                            for (std::size_t bi = 0; bi != group.image_patch_resource_idx.size(); ++bi)
                            {
                                if (group.image_patch_resource_idx[bi] != ri)
                                    continue;
                                const auto &barrier = group.image_barriers[bi];
                                const auto writes = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                                const auto reads = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT;
                                if ((barrier.srcAccessMask & writes) && (barrier.dstAccessMask & reads))
                                    ++stats.shared_write_sample;
                                if (bi < group.image_src_is_final_state.size() && group.image_src_is_final_state[bi])
                                    ++stats.shared_cross_view;
                            }
                    }
                });
            }
#endif

            [[nodiscard]] render::Expected<void> rebuildRenderer()
            {
                auto *provider = swapchainProvider();
                if (provider == nullptr || provider->imageCount() == 0U)
                    return {};
                state_.textures->clear();
                state_.textures->renderer = nullptr;
                state_.renderer.reset();
                auto &resources = resourceContext();
                auto renderer = lux::ui::detail::UiVulkanRenderer::create(
                    lux::ui::detail::UiVulkanRendererCreateInfo{
                        resources.instanceContext().instance(), resources.physicalDevice(), resources.logicalDevice(),
                        resources.graphicsQueueFamilyIndex(), resources.graphicsQueue(), provider->format(),
                        provider->imageCount(), resources.instanceContext().allocator()},
                    state_.font);
                if (!renderer)
                    return render::renderFailure<render::err::device::VulkanObjectCreationFailed>();
                state_.renderer = std::move(*renderer);
                state_.textures->renderer = state_.renderer.get();
                state_.renderer->setTextureResolver(&ServerState::resolve, &state_);
                return {};
            }

            ServerState state_;
            std::shared_ptr<render::RenderChannelSync> sync_;
            std::atomic<std::uint64_t> *failed_packet_{};
        };
    } // namespace

#if defined(LUX_EDITOR_RENDERER_TEST_DIAGNOSTICS)
    namespace
    {
        thread_local EStartupFault startup_fault{};
        thread_local render::RenderError startup_injected_error{};
        std::atomic<unsigned> workers_started{}, servers_created{}, servers_initialized{}, servers_attached{};
        std::atomic<unsigned> servers_destroyed{}, workers_exited{};
        struct ServerDestructionTrace final
        {
            bool constructed{};
            ~ServerDestructionTrace()
            {
                if (constructed)
                    ++servers_destroyed;
            }
        };
    }
    void RendererTestAccess::failStartup(EStartupFault fault, render::RenderError error) noexcept
    {
        startup_fault = fault;
        startup_injected_error = error;
        workers_started = servers_created = servers_initialized = 0;
        servers_attached = servers_destroyed = workers_exited = 0;
    }
    StartupTrace RendererTestAccess::startupTrace() noexcept
    {
        return {workers_started.load(), servers_created.load(), servers_initialized.load(), servers_attached.load(),
                servers_destroyed.load(), workers_exited.load()};
    }
#endif

    RenderResult<std::jthread> startRendererThread(RendererThread &state, lux::window::LuxWindow &window,
                                                   lux::ui::detail::UiFontAtlasSnapshot font,
                                                   const RendererConfig &config) noexcept
    {
        try
        {
#if defined(LUX_EDITOR_RENDERER_TEST_DIAGNOSTICS)
            const auto fault = std::exchange(startup_fault, EStartupFault::NONE);
            const auto injected_error = startup_injected_error;
            if (fault == EStartupFault::THREAD_LAUNCH)
                return lux::cxx::unexpected(RendererFailure{ERendererError::EXTERNAL_FAILURE, injected_error});
#endif
            const auto required = lux::window::LuxWindow::requiredVulkanInstanceExtensions();
            std::vector<const char *> extensions(required.begin(), required.end());
#if defined(LUX_EDITOR_RENDERER_TEST_DIAGNOSTICS)
            if (fault == EStartupFault::INITIALIZE)
                extensions.push_back("VK_LUX_er1_deliberately_unavailable_startup_extension");
#endif
            return std::jthread([&state, &window,
#if defined(LUX_EDITOR_RENDERER_TEST_DIAGNOSTICS)
                                 fault, injected_error,
#endif
                                 font = std::move(font), extensions = std::move(extensions),
                                 validation = config.validation, sink = config.validation_message_sink]() mutable {
#if defined(LUX_EDITOR_RENDERER_TEST_DIAGNOSTICS)
                ++workers_started;
#endif
                try
                {
#if defined(LUX_EDITOR_RENDERER_TEST_DIAGNOSTICS)
                    ServerDestructionTrace destruction_trace;
#endif
                    EditorRenderServer server(state.frames, state.controls, state.uploads, state.sync);
#if defined(LUX_EDITOR_RENDERER_TEST_DIAGNOSTICS)
                    ++servers_created;
                    destruction_trace.constructed = true;
#endif
                    render::ServerConfig server_config;
                    server_config.instance_extensions = std::move(extensions);
                    server_config.enable_validation = validation;
                    server_config.validation_error_counter = &state.statistics->validation_errors;
                    server_config.validation_message_sink = std::move(sink);
                    auto initialized = server.initialize(std::move(server_config), std::move(font), state.catalog,
                                                         state.statistics, state.sync, state.failed_packet);
#if defined(LUX_EDITOR_RENDERER_TEST_DIAGNOSTICS)
                    if (initialized)
                    {
                        ++servers_initialized;
                        if (fault == EStartupFault::AFTER_DEVICE)
                            initialized = lux::cxx::unexpected(injected_error);
                    }
#endif
                    if (initialized)
                    {
                        initialized = server.attach(window);
#if defined(LUX_EDITOR_RENDERER_TEST_DIAGNOSTICS)
                        if (initialized)
                        {
                            ++servers_attached;
                            if (fault == EStartupFault::AFTER_ATTACH)
                                initialized = lux::cxx::unexpected(injected_error);
                        }
#endif
                    }
                    if (!initialized)
                    {
                        state.startup_error = initialized.error();
                        state.startup.store(2, std::memory_order_release);
                    }
                    else
                    {
                        state.submit_operation = server.submitOperation();
                        state.startup.store(1, std::memory_order_release);
                    }
                    state.startup.notify_all();
                    if (initialized)
                        for (;;)
                        {
#if defined(LUX_EDITOR_RENDERER_TEST_DIAGNOSTICS)
                            // Test-only rendezvous before the real consumer enters its next tick.
                            // Queues, messages, FIFO and reply generation remain unchanged.
                            if (state.pause_requested.load(std::memory_order_acquire))
                            {
                                state.pause_reached.store(true, std::memory_order_release);
                                state.pause_requested.wait(true, std::memory_order_acquire);
                                state.pause_reached.store(false, std::memory_order_release);
                            }
#endif
                            if (!server.tick())
                                break;
                        }
                }
                catch (const std::bad_alloc &)
                {
                    state.allocation_failed.store(true, std::memory_order_release);
                    state.startup.store(2, std::memory_order_release);
                    state.startup.notify_all();
                    ++state.statistics->events;
                }
                // Backend destruction has completed on its thread before this terminal fact is published.
                state.sync->requestStop();
                state.stopped.store(1, std::memory_order_release);
#if defined(LUX_EDITOR_RENDERER_TEST_DIAGNOSTICS)
                ++workers_exited;
#endif
            });
        }
        catch (const std::bad_alloc &)
        {
            return lux::cxx::unexpected(RendererFailure{ERendererError::ALLOCATION_FAILURE});
        }
        catch (const std::system_error &)
        {
            return lux::cxx::unexpected(RendererFailure{ERendererError::EXTERNAL_FAILURE});
        }
    }
} // namespace lux::editor::rendering::detail
