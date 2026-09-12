#include <lux/engine/editor/rendering/detail/RendererThread.hpp>
#include <lux/engine/editor/rendering/detail/ReplyPump.hpp>
#include <lux/engine/editor/rendering/detail/RenderFrameQueue.hpp>
#include <lux/engine/editor/rendering/detail/ViewImageLifetime.hpp>
#include <lux/engine/function/render/client/genops/ViewCameraOperation.ops.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <lux/engine/ui/UISession.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <new>
#if defined(LUX_EDITOR_RENDERER_TEST_DIAGNOSTICS)
#include <lux/engine/editor/rendering/detail/RendererTestAccess.hpp>
#endif

namespace lux::editor::rendering
{
    namespace
    {
        std::atomic<std::uint64_t> next_renderer{1}, next_image{1};
        std::uint64_t issue(std::atomic<std::uint64_t> &sequence) noexcept
        {
            auto value = sequence.load(std::memory_order_relaxed);
            while (value != (std::numeric_limits<std::uint64_t>::max)())
                if (sequence.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
                    return value;
            return 0;
        }
        auto fail(ERendererError code) noexcept
        {
            return lux::cxx::unexpected(RendererFailure{code});
        }
        template <class T, std::size_t Slots>
        bool clearJoinedRing(lux::cxx::BoundedSpscFrameRing<T, Slots> &ring) noexcept
        {
            // The worker has joined, so both ring roles are exclusively owned here. Published slots
            // are only part of its storage: free historical slots also keep their last attachments.
            ring.currentRead().clear_keep_capacity();
            while (ring.tryAcquireRead())
                ring.currentRead().clear_keep_capacity();
            for (std::size_t i = 0; i < Slots; ++i)
            {
                auto *slot = ring.tryBeginWrite();
                if (!slot)
                    return false;
                slot->clear_keep_capacity();
                // Advance empty storage locally. No server remains and no command is executed.
                if (!ring.publishWrite() || !ring.tryAcquireRead())
                    return false;
            }
            return true;
        }

        class UploadQueue final
        {
        public:
            UploadQueue(std::size_t count, std::size_t bytes) : pending_(count), limit_(bytes)
            {
            }
            static lux::render::UploadSubmitNoReplyResult submit(
                void *owner, std::shared_ptr<lux::render::detail::PreparedUpload> packet) noexcept
            {
                auto &queue = *static_cast<UploadQueue *>(owner);
                std::lock_guard lock{queue.mutex_};
                if (!queue.accepting_)
                    return lux::cxx::unexpected(lux::render::ERenderUploadSubmitError::STOPPING);
                if (queue.size_ == queue.pending_.size())
                    return lux::cxx::unexpected(lux::render::ERenderUploadSubmitError::QUEUE_FULL);
                const auto bytes = packet->packet.accountedBytes();
                if (bytes > queue.limit_ - queue.bytes_)
                    return lux::cxx::unexpected(lux::render::ERenderUploadSubmitError::BYTE_BUDGET_EXHAUSTED);
                queue.bytes_ += bytes;
                queue.pending_[(queue.head_ + queue.size_++) % queue.pending_.size()] = std::move(packet);
                return {};
            }
            void stop() noexcept
            {
                std::lock_guard lock{mutex_};
                accepting_ = false;
            }
            bool empty() const noexcept
            {
                std::lock_guard lock{mutex_};
                return size_ == 0;
            }
            std::size_t poll(lux::render::RenderUploadSession &uploads, bool stopped, std::size_t budget)
            {
                std::size_t processed{};
                while (processed < budget)
                {
                    std::shared_ptr<lux::render::detail::PreparedUpload> packet;
                    {
                        std::lock_guard lock{mutex_};
                        if (!size_)
                            break;
                        packet = pending_[head_];
                    }
                    const auto accounted_bytes = packet->packet.accountedBytes();
                    if (stopped)
                        static_cast<void>(packet->callback.settleFailure(
                            lux::render::renderError<lux::render::err::comm::ChannelStopping>()));
                    else
                    {
                        lux::render::ReplyDispatchCallback callback{
                            [packet](auto reply, const auto &record) { packet->callback(reply, record); },
                            [packet](auto error) { static_cast<void>(packet->callback.settleFailure(error)); }};
                        const auto submitted =
                            packet->expected_reply_type == lux::render::kInvalidTypeId
                                ? uploads.trySubmitPreparedNoReply(packet->packet)
                                : uploads.trySubmitPrepared(packet->packet, packet->expected_reply_type,
                                                            std::move(callback));
                        if (!submitted)
                            break;
                    }
                    std::lock_guard lock{mutex_};
                    bytes_ -= accounted_bytes;
                    pending_[head_].reset();
                    head_ = (head_ + 1) % pending_.size();
                    --size_;
                    ++processed;
                }
                return processed;
            }

        private:
            mutable std::mutex mutex_;
            std::vector<std::shared_ptr<lux::render::detail::PreparedUpload>> pending_;
            std::size_t limit_{}, head_{}, size_{}, bytes_{};
            bool accepting_{true};
        };
    } // namespace

    struct EditorRenderer::Impl final
    {
        explicit Impl(const RendererConfig &value)
            : config(value), frames(value.frame_capacity), views(value.view_capacity),
              diagnostics(value.diagnostic_capacity)
        {
        }
        const std::thread::id owner{std::this_thread::get_id()};
        RendererConfig config;
        std::size_t next_reply_lane{};
        detail::RendererThread thread;
        std::jthread worker;
        std::unique_ptr<lux::render::RenderProgramSession> programs;
        std::unique_ptr<lux::render::RenderControlSession> control;
        std::unique_ptr<lux::render::RenderUploadSession> uploads;
        std::shared_ptr<UploadQueue> upload_queue;
        lux::render::RenderUploadClient upload_client;
        detail::RenderFrameQueue frames;
        std::vector<RenderView *> views;
        std::vector<RendererDiagnostic> diagnostics;
        std::size_t diagnostic_head{}, diagnostic_count{};
        bool terminal_diagnostic{};
        std::optional<RendererDiagnostic> pending_terminal_diagnostic;
        void recordDiagnostic(RendererDiagnostic value) noexcept
        {
            if (value.terminal)
            {
                pending_terminal_diagnostic = value;
                return;
            }
            if (diagnostic_count == diagnostics.size())
            {
                thread.statistics->dropped += value.occurrences;
                return;
            }
            diagnostics[(diagnostic_head + diagnostic_count++) % diagnostics.size()] = value;
        }
        std::uint64_t identity{}, sequence{};
        std::size_t leases{};
        bool closing{}, joined{}, busy{}, terminal_frames_released{};
        RenderResult<void> check() const noexcept
        {
            if (owner != std::this_thread::get_id())
                return fail(ERendererError::WRONG_THREAD);
            if (busy)
                return fail(ERendererError::BUSY);
            return {};
        }
    };

    EditorRenderer::EditorRenderer(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
    {
    }
    EditorRenderer::~EditorRenderer() noexcept
    {
        if (!impl_->joined)
            std::terminate();
    }
    RenderResult<std::unique_ptr<EditorRenderer>> EditorRenderer::create(lux::window::LuxWindow &window,
                                                                         lux::ui::UISession &ui,
                                                                         const RendererConfig &config) noexcept
    {
        if (!ui.dispatcherRef().isCurrent())
            return fail(ERendererError::WRONG_THREAD);
        const bool valid_capacity =
            config.frame_capacity >= 2 && config.frame_capacity <= 1024 && config.control_capacity >= 2 &&
            config.control_capacity <= 65536 && config.upload_capacity >= 2 && config.upload_capacity <= 65536 &&
            config.upload_byte_capacity && config.view_capacity && config.view_capacity <= 48 &&
            config.texture_capacity >= config.view_capacity * 2 && config.texture_capacity <= 48 &&
            config.diagnostic_capacity && config.diagnostic_capacity <= 65536;
        if (!valid_capacity || !window.isInitialized())
            return fail(ERendererError::INVALID_ARGUMENT);
        try
        {
            auto impl = std::make_unique<Impl>(config);
            impl->identity = issue(next_renderer);
            if (!impl->identity)
                return fail(ERendererError::CAPACITY);
            auto &thread = impl->thread;
            thread.frames = lux::render::RenderProgramChannel<>::create(config.frame_capacity);
            thread.controls = lux::render::RenderControlChannel<>::create(config.control_capacity);
            thread.uploads =
                lux::render::RenderUploadChannel<>::create(config.upload_capacity, config.upload_byte_capacity);
            thread.sync = std::make_shared<lux::render::RenderChannelSync>();
            thread.statistics = std::make_shared<detail::RenderStatistics>();
            impl->programs = std::make_unique<lux::render::RenderProgramSession>(thread.frames, thread.sync);
            impl->control = std::make_unique<lux::render::RenderControlSession>(thread.controls, thread.sync);
            impl->uploads = std::make_unique<lux::render::RenderUploadSession>(thread.uploads, thread.sync);
            impl->upload_queue = std::make_shared<UploadQueue>(config.upload_capacity, config.upload_byte_capacity);
            impl->upload_client = lux::render::RenderUploadClient::bind(impl->upload_queue, &UploadQueue::submit);
            impl->programs->setErrorEventHandler(
                [stats = thread.statistics](const auto &batch) { stats->dropped += batch.dropped; },
                [owner = impl.get()](const auto &event) {
                    owner->thread.statistics->events += event.occurrences;
                    owner->recordDiagnostic({{ERendererError::DEVICE_FAILURE, event.error},
                                             event.scene_index,
                                             event.occurrences,
                                             event.frame_serial,
                                             event.seq,
                                             false});
                });
            auto font = lux::ui::detail::captureUiFontAtlas(ui);
            if (!font)
            {
                const auto code = font.error() == lux::ui::EUiInitError::ALLOCATION_FAILURE
                                      ? ERendererError::ALLOCATION_FAILURE
                                      : (font.error() == lux::ui::EUiInitError::WRONG_THREAD
                                             ? ERendererError::WRONG_THREAD : ERendererError::INVALID_ARGUMENT);
                return fail(code);
            }
            // Allocate the final owner before starting the worker. After startup, adopting the thread cannot fail.
            auto result = std::unique_ptr<EditorRenderer>(new EditorRenderer(std::move(impl)));
            result->impl_->joined = true;
            auto started = detail::startRendererThread(result->impl_->thread, window, std::move(*font), config);
            if (!started)
                return lux::cxx::unexpected(started.error());
            result->impl_->worker = std::move(*started);
            while (thread.startup.load(std::memory_order_acquire) == 0)
                thread.startup.wait(0, std::memory_order_acquire);
            if (thread.startup.load(std::memory_order_acquire) != 1)
            {
                result->impl_->worker.join();
                return lux::cxx::unexpected(RendererFailure{thread.allocation_failed.load()
                                                                ? ERendererError::ALLOCATION_FAILURE
                                                                : ERendererError::DEVICE_FAILURE,
                                                            thread.startup_error});
            }
            result->impl_->joined = false;
            return result;
        }
        catch (const std::bad_alloc &)
        {
            return fail(ERendererError::ALLOCATION_FAILURE);
        }
    }
    ERendererState EditorRenderer::state() const noexcept
    {
        if (impl_->thread.stopped.load(std::memory_order_acquire))
            return impl_->thread.sync->terminalError().ok() && impl_->thread.startup.load() == 1
                       ? ERendererState::STOPPED
                       : ERendererState::FAILED;
        return impl_->closing || impl_->thread.sync->isStopping() ? ERendererState::STOPPING : ERendererState::READY;
    }
    RenderResult<std::optional<RendererDiagnostic>> EditorRenderer::takeDiagnostic() noexcept
    {
        if (auto check = impl_->check(); !check)
            return lux::cxx::unexpected(check.error());
        if (!impl_->diagnostic_count)
            return std::exchange(impl_->pending_terminal_diagnostic, std::nullopt);
        auto result = impl_->diagnostics[impl_->diagnostic_head];
        impl_->diagnostic_head = (impl_->diagnostic_head + 1) % impl_->diagnostics.size();
        --impl_->diagnostic_count;
        return std::optional{result};
    }
    RendererStatistics EditorRenderer::statistics() const noexcept
    {
        const auto &stats = *impl_->thread.statistics;
        return {stats.frames.load(),
                stats.slots.load(),
                stats.created.load(),
                stats.retired.load(),
                stats.misses.load(),
                stats.events.load(),
                stats.dropped.load(),
                stats.completed.load(),
                impl_->leases,
                static_cast<std::size_t>(
                    std::count_if(impl_->views.begin(), impl_->views.end(), [](auto *v) { return v; })),
                impl_->frames.size(),
                static_cast<std::uint64_t>(stats.validation_errors.load())};
    }
    RenderResult<RendererCloseStatus> EditorRenderer::closeStatus() const noexcept
    {
        if (auto checked = impl_->check(); !checked)
            return lux::cxx::unexpected(checked.error());
        RendererCloseStatus result;
        result.statistics = statistics();
        result.state = state();
        result.close_requested = impl_->closing;
        result.uploads_pending = !impl_->upload_queue->empty();
        result.program_pending = impl_->programs && impl_->programs->hasPendingSubmit();
        result.worker_stopped = impl_->thread.stopped.load(std::memory_order_acquire);
        result.scene_releases = impl_->control->pendingSceneReleases();
        result.view_releases = impl_->control->pendingViewReleases();
        result.target_releases = impl_->control->pendingTargetReleases();
        for (const auto *view : impl_->views)
        {
            if (!view)
                continue;
            auto current = view->closeStatus();
            if (!current)
                return lux::cxx::unexpected(current.error());
            result.views[result.view_count++] = *current;
        }
        return result;
    }
    RenderResult<ImageContentStamp> EditorRenderer::imageEvidence(const ViewImage &image) const noexcept
    {
        if (auto checked = impl_->check(); !checked)
            return lux::cxx::unexpected(checked.error());
        const auto *record = detail::ViewImageAccess::record(image);
        if (!record || !record->version || image.view.renderer != impl_->identity)
            return fail(ERendererError::STALE_IMAGE);
        const auto &version = *record->version;
        const bool valid = image.view == version.id && image.extent == version.extent &&
                           image.texture == version.texture && image.content.source == record->content.source &&
                           image.content.frame_serial == record->content.frame_serial &&
                           image.content.evidence == record->content.evidence;
        if (!valid)
            return fail(ERendererError::STALE_IMAGE);
        auto result = record->content;
        const auto submitted = record->submitted.load(std::memory_order_acquire);
        if (submitted)
        {
            result.frame_serial = submitted;
            result.evidence = impl_->thread.statistics->completed.load(std::memory_order_acquire) >= submitted
                                  ? EImageEvidence::GPU_COMPLETE
                                  : EImageEvidence::RECORDED;
        }
        return result;
    }
    bool EditorRenderer::controlAvailable(std::size_t packets) const noexcept
    {
        if (impl_->owner != std::this_thread::get_id() || impl_->thread.sync->isStopping())
            return false;
        const auto &queue = impl_->thread.controls->requests;
        return packets <= queue.capacity() - queue.size();
    }
    RenderResult<std::unique_ptr<RenderView>> EditorRenderer::openView(lux::render::RenderSceneId scene,
                                                                       ViewConfig config) noexcept
    {
        if (auto check = impl_->check(); !check)
            return lux::cxx::unexpected(check.error());
        if (state() != ERendererState::READY)
            return fail(ERendererError::STOPPING);
        if (!scene.isValid() || !config.sampled || config.extent.width > 16384 || config.extent.height > 16384)
            return fail(ERendererError::INVALID_ARGUMENT);
        const bool invalid_page_size = !std::isfinite(config.coordinate_page_size) ||
                                       config.coordinate_page_size <= 0 ||
                                       config.coordinate_page_size > (std::numeric_limits<float>::max)() ||
                                       static_cast<float>(config.coordinate_page_size) <= 0;
        if (invalid_page_size)
            return fail(ERendererError::INVALID_ARGUMENT);
        const auto slot = std::find(impl_->views.begin(), impl_->views.end(), nullptr);
        if (slot == impl_->views.end())
            return fail(ERendererError::CAPACITY);
        const auto generation = issue(next_image);
        if (!generation)
            return fail(ERendererError::CAPACITY);
        const RenderViewId id{impl_->identity, static_cast<std::uint64_t>(slot - impl_->views.begin()), generation};
        auto view = RenderView::create(*this, *impl_->control, id, scene, config);
        if (view)
            *slot = view->get();
        return view;
    }
    RenderResult<EditorFramePacket> EditorRenderer::sealFrame(lux::ui::UiFrameSnapshot &snapshot,
                                                              std::span<const ViewImage> images) noexcept
    {
        if (auto check = impl_->check(); !check)
            return lux::cxx::unexpected(check.error());
        if (state() != ERendererState::READY)
            return fail(ERendererError::STOPPING);
        if (!snapshot.valid())
            return fail(ERendererError::INVALID_ARGUMENT);
        if (impl_->sequence == (std::numeric_limits<std::uint64_t>::max)())
            return fail(ERendererError::CAPACITY);
        if (images.size() > impl_->config.texture_capacity)
            return fail(ERendererError::CAPACITY);
        for (std::size_t index = 0; index < images.size(); ++index)
        {
            const auto &image = images[index];
            const auto *record = detail::ViewImageAccess::record(image);
            if (!record || !record->version || image.view.renderer != impl_->identity)
                return fail(ERendererError::STALE_IMAGE);
            const auto &version = *record->version;
            const bool valid_reference =
                image.view == version.id && image.texture == version.texture && image.extent == version.extent &&
                image.content.source == record->content.source && image.content.evidence == record->content.evidence &&
                image.content.frame_serial == record->content.frame_serial;
            if (!valid_reference)
                return fail(ERendererError::STALE_IMAGE);
            if (image.view.slot >= impl_->views.size() || !impl_->views[image.view.slot] ||
                impl_->views[image.view.slot]->id() != image.view)
                return fail(ERendererError::STALE_VIEW);
            const auto view_state = impl_->views[image.view.slot]->status().state;
            if (view_state == EViewState::CLOSING || view_state == EViewState::CLOSED ||
                view_state == EViewState::FAILED)
                return fail(ERendererError::STALE_VIEW);
            for (std::size_t previous = 0; previous < index; ++previous)
                if (images[previous].texture == image.texture &&
                    !detail::ViewImageAccess::sameRecord(images[previous], image))
                    return fail(ERendererError::STALE_IMAGE);
        }
        for (const auto token : snapshot.textures())
        {
            const bool missing =
                std::none_of(images.begin(), images.end(), [&](const auto &image) { return image.texture == token; });
            if (missing)
                return fail(ERendererError::INCOMPLETE_FRAME_REFERENCES);
        }
        try
        {
            auto storage = std::make_unique<EditorFramePacket::Storage>();
            storage->renderer = impl_->identity;
            lux::render::RenderProgramSession::Builder builder(storage->program);
            builder.begin(impl_->config.program_memory);
            storage->program.kind = lux::render::ERenderProgramKind::Frame;
            storage->program.attachments.reserve(1);
            const auto attachment = builder.emplaceAttachment<detail::FrameDrawData>(detail::kUiDrawAttachment);
            auto &data = *static_cast<detail::FrameDrawData *>(storage->program.attachments[attachment].object);
            data.images.assign(images.begin(), images.end());
            const auto operation = impl_->thread.catalog.ops<lux::render::ViewCameraOperationIds>("StandardViewCamera")
                                       .id<lux::render::ViewCameraUpdateOp>();
            for (const auto &image : data.images)
            {
                const auto *record = detail::ViewImageAccess::record(image);
                builder.pushBulk(operation,
                                 std::span<const lux::render::ViewCameraUpdatePayload>{&record->wire_camera, 1});
            }
            builder.push(lux::render::opcodes::CommandOp, impl_->thread.submit_operation,
                         detail::SubmitDrawPayload{attachment});
            if (!builder.valid())
                return fail(ERendererError::CAPACITY);
            storage->sequence = ++impl_->sequence;
            data.packet_sequence = storage->sequence;
            data.snapshot = std::move(snapshot);
            return EditorFramePacket{std::move(storage)};
        }
        catch (const std::bad_alloc &)
        {
            return fail(ERendererError::ALLOCATION_FAILURE);
        }
    }
    RenderResult<EFrameSubmit> EditorRenderer::trySubmitFrame(EditorFramePacket &packet) noexcept
    {
        if (auto check = impl_->check(); !check)
            return lux::cxx::unexpected(check.error());
        if (state() != ERendererState::READY)
            return fail(ERendererError::STOPPING);
        if (!packet.valid() || packet.storage_->renderer != impl_->identity)
            return fail(ERendererError::INVALID_ARGUMENT);
        if (impl_->frames.full())
            return EFrameSubmit::BACKPRESSURED;
        impl_->frames.accept(packet);
        return EFrameSubmit::SUBMITTED;
    }

    RenderResult<std::size_t> EditorRenderer::poll(std::size_t budget) noexcept
    {
        if (auto check = impl_->check(); !check)
            return lux::cxx::unexpected(check.error());
        if (!budget)
            return std::size_t{0};
        struct PollGate final
        {
            bool &busy;
            ~PollGate() noexcept
            {
                busy = false;
            }
        } gate{impl_->busy};
        impl_->busy = true;
        const auto count = detail::pumpRendererReplies(*impl_->control, *impl_->programs, *impl_->uploads,
                                                       impl_->next_reply_lane, budget);
        const auto terminal_error = impl_->thread.sync->terminalError();
        const bool allocation_failed = impl_->thread.allocation_failed.load(std::memory_order_acquire);
        if (!impl_->terminal_diagnostic && (!terminal_error.ok() || allocation_failed))
        {
            impl_->recordDiagnostic(
                {{allocation_failed ? ERendererError::ALLOCATION_FAILURE : ERendererError::DEVICE_FAILURE,
                  terminal_error,
                  {},
                  impl_->thread.failed_packet.load(std::memory_order_acquire)},
                 lux::render::RenderErrorEvent::kNoScene,
                 1,
                 0,
                 0,
                 true});
            impl_->terminal_diagnostic = true;
        }
        try
        {
            static_cast<void>(impl_->upload_queue->poll(*impl_->uploads, impl_->thread.sync->isStopping(), budget));
        }
        catch (const std::bad_alloc &)
        {
            return fail(ERendererError::ALLOCATION_FAILURE);
        }
        if (impl_->thread.stopped.load(std::memory_order_acquire) && !impl_->terminal_frames_released)
        {
            // The consumer is gone: reclaim accepted CPU packets before waiting for externally held images.
            // Keep the program endpoint alive until all Scene runtime leases have been returned.
            impl_->frames.cancel();
            if (impl_->worker.joinable())
                impl_->worker.join();
            if (!clearJoinedRing(impl_->thread.frames->requests))
                return fail(ERendererError::CONTRACT_FAILURE);
            impl_->terminal_frames_released = true;
        }
        if (!impl_->terminal_frames_released)
            static_cast<void>(impl_->programs->retryPendingSubmit());
        if (!impl_->frames.empty() && impl_->programs->trySubmitPrepared(impl_->frames.front().storage_->program))
        {
            impl_->frames.pop();
        }
        bool maintenance = impl_->closing;
        for (auto *&view : impl_->views)
        {
            if (!view)
                continue;
            const auto result = view->pollResources();
            if (!result)
                return lux::cxx::unexpected(result.error());
            if (auto failure = view->takeFailure())
            {
                ++impl_->thread.statistics->events;
                impl_->recordDiagnostic({*failure});
            }
            const auto state = view->status().state;
            maintenance |=
                state == EViewState::CLOSING || state == EViewState::RESIZING || state == EViewState::SUSPENDED;
            if (state == EViewState::CLOSED)
                view = nullptr;
        }
        if (maintenance && impl_->frames.empty() && !impl_->programs->hasPendingSubmit() &&
            !impl_->thread.sync->isStopping())
        {
            // A finite renderer-owned empty frame advances its own fences and retires old draw attachments.
            try
            {
                lux::render::RenderProgram<> program;
                lux::render::RenderProgramSession::Builder builder(program);
                builder.begin(impl_->config.program_memory);
                program.kind = lux::render::ERenderProgramKind::Frame;
                program.attachments.reserve(1);
                const auto attachment = builder.emplaceAttachment<detail::FrameDrawData>(detail::kUiDrawAttachment);
                builder.push(lux::render::opcodes::CommandOp, impl_->thread.submit_operation,
                             detail::SubmitDrawPayload{attachment});
                static_cast<void>(impl_->programs->trySubmitPrepared(program));
            }
            catch (const std::bad_alloc &)
            {
                return fail(ERendererError::ALLOCATION_FAILURE);
            }
        }
        return count;
    }
    RenderResult<void> EditorRenderer::beginClose() noexcept
    {
        if (auto check = impl_->check(); !check)
            return check;
        impl_->closing = true;
#if defined(LUX_EDITOR_RENDERER_TEST_DIAGNOSTICS)
        impl_->thread.pause_requested.store(false, std::memory_order_release);
        impl_->thread.pause_requested.notify_all();
#endif
        impl_->upload_queue->stop();
        return {};
    }
    RenderResult<ERenderClose> EditorRenderer::advanceClose() noexcept
    {
        if (auto check = impl_->check(); !check)
            return lux::cxx::unexpected(check.error());
        if (!impl_->closing)
            return fail(ERendererError::BUSY);
        if (auto result = poll(64); !result)
            return lux::cxx::unexpected(result.error());
        const bool has_views = std::any_of(impl_->views.begin(), impl_->views.end(), [](auto *view) { return view; });
        if (impl_->leases || has_views)
            return ERenderClose::PENDING;
        // The stopped backend has already destroyed its GPU resources. Deferred release counters
        // and an unpublished endpoint flag cannot produce replies after that terminal fact.
        if (impl_->terminal_frames_released && impl_->upload_queue->empty())
            return ERenderClose::COMPLETE;
        if (!impl_->upload_queue->empty() || !impl_->frames.empty() || impl_->programs->hasPendingSubmit())
            return ERenderClose::PENDING;
        const auto released = impl_->control->flushDeferredReleases();
        if (!released)
        {
            RendererFailure error{ERendererError::ALLOCATION_FAILURE};
            error.render_error = released.error();
            return lux::cxx::unexpected(error);
        }
        if (!*released || impl_->control->pendingSceneReleases() || impl_->control->pendingViewReleases() ||
            impl_->control->pendingTargetReleases())
            return ERenderClose::PENDING;
        impl_->thread.sync->requestStop();
        return impl_->thread.stopped.load(std::memory_order_acquire) ? ERenderClose::COMPLETE : ERenderClose::PENDING;
    }
    RenderResult<void> EditorRenderer::joinStopped() noexcept
    {
        if (auto check = impl_->check(); !check)
            return check;
        if (impl_->joined)
            return {};
        if (!impl_->thread.stopped.load(std::memory_order_acquire) || impl_->leases ||
            std::any_of(impl_->views.begin(), impl_->views.end(), [](auto *view) { return view; }))
            return fail(ERendererError::BUSY);
        if (impl_->worker.joinable())
            impl_->worker.join();
        impl_->frames.cancel();
        impl_->programs.reset();
        if (!clearJoinedRing(impl_->thread.frames->requests))
            return fail(ERendererError::CONTRACT_FAILURE);
        impl_->joined = true;
        return {};
    }
    lux::cxx::expected<lux::scene::RenderRuntimeLease, lux::scene::RenderRuntimeFailure> EditorRenderer::
        acquire() noexcept
    {
        if (impl_->owner != std::this_thread::get_id() || impl_->leases == (std::numeric_limits<std::size_t>::max)())
            return lux::cxx::unexpected(
                lux::scene::RenderRuntimeFailure{lux::scene::ERenderRuntimeError::ACTIVATION_FAILURE});
        if (state() != ERendererState::READY)
            return lux::cxx::unexpected(lux::scene::RenderRuntimeFailure{lux::scene::ERenderRuntimeError::STOPPING});
        ++impl_->leases;
        return makeLease();
    }
    void EditorRenderer::release() noexcept
    {
        --impl_->leases;
    }
    lux::render::RenderControlSession &EditorRenderer::control() noexcept
    {
        return *impl_->control;
    }
    lux::render::RenderProgramSession &EditorRenderer::programs() noexcept
    {
        return *impl_->programs;
    }
    lux::render::RenderUploadClient EditorRenderer::upload() noexcept
    {
        return impl_->upload_client;
    }
    const lux::render::FeatureCatalog &EditorRenderer::features() const noexcept
    {
        return impl_->thread.catalog;
    }
#if defined(LUX_EDITOR_RENDERER_TEST_DIAGNOSTICS)
    RenderResult<void> detail::RendererTestAccess::rejectMaterialUpload(
        EditorRenderer &renderer, lux::render::ShaderHandle forward, lux::render::ShaderHandle gbuffer) noexcept
    {
        if (auto checked = renderer.impl_->check(); !checked)
            return checked;
        auto &stats = *renderer.impl_->thread.statistics;
        if (stats.reject_material.load(std::memory_order_acquire))
            return fail(ERendererError::BUSY);
        stats.rejected_forward = forward;
        stats.rejected_gbuffer = gbuffer;
        stats.reject_material.store(true, std::memory_order_release);
        return {};
    }
    RenderResult<std::uint64_t>
    detail::RendererTestAccess::rejectedMaterialUploads(const EditorRenderer &renderer) noexcept
    {
        if (auto checked = renderer.impl_->check(); !checked)
            return lux::cxx::unexpected(checked.error());
        return renderer.impl_->thread.statistics->material_rejections.load(std::memory_order_acquire);
    }
    RenderResult<void> detail::RendererTestAccess::observeResourceMemory(EditorRenderer &renderer) noexcept
    {
        if (auto checked = renderer.impl_->check(); !checked)
            return checked;
        renderer.impl_->thread.statistics->observe_memory.store(true, std::memory_order_release);
        return {};
    }
    RenderResult<detail::ResourceMemoryTrace>
    detail::RendererTestAccess::resourceMemoryTrace(const EditorRenderer &renderer) noexcept
    {
        if (auto checked = renderer.impl_->check(); !checked)
            return lux::cxx::unexpected(checked.error());
        return renderer.impl_->thread.statistics->memory_trace.load(std::memory_order_acquire);
    }
    RenderResult<void> detail::RendererTestAccess::observeSharedImports(EditorRenderer &renderer) noexcept
    {
        if (auto checked = renderer.impl_->check(); !checked)
            return checked;
        renderer.impl_->thread.statistics->observe_shared.store(true, std::memory_order_release);
        return {};
    }
    RenderResult<detail::SharedImportTrace>
    detail::RendererTestAccess::sharedImportTrace(const EditorRenderer &renderer) noexcept
    {
        if (auto checked = renderer.impl_->check(); !checked)
            return lux::cxx::unexpected(checked.error());
        const auto &s = *renderer.impl_->thread.statistics;
        return SharedImportTrace{s.shared_pairs.load(), s.shared_write_sample.load(), s.shared_cross_view.load(),
                                 s.shared_reads.load(), s.shared_writes.load(), s.shared_first_serial.load(),
                                 s.shared_image.load()};
    }
    RenderResult<void> detail::RendererTestAccess::pauseConsumer(EditorRenderer &renderer, bool paused) noexcept
    {
        if (auto checked = renderer.impl_->check(); !checked)
            return checked;
        auto &thread = renderer.impl_->thread;
        if (renderer.impl_->closing || thread.stopped.load(std::memory_order_acquire))
            return fail(ERendererError::STOPPING);
        thread.pause_requested.store(paused, std::memory_order_release);
        thread.pause_requested.notify_all();
        thread.sync->notifyRequestStateChanged();
        return {};
    }
    bool detail::RendererTestAccess::consumerPaused(const EditorRenderer &renderer) noexcept
    {
        if (renderer.impl_->owner != std::this_thread::get_id())
            return false;
        return renderer.impl_->thread.pause_reached.load(std::memory_order_acquire);
    }
#endif
} // namespace lux::editor::rendering
