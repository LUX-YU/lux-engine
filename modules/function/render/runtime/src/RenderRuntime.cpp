#include <lux/engine/render/RenderRuntime.hpp>
#include <lux/engine/render/detail/RendererThread.hpp>
#include <lux/engine/render/detail/ReplyPump.hpp>
#include <lux/engine/render/detail/UploadQueue.hpp>
#include <lux/engine/render/detail/ViewResources.hpp>

#include <cmath>
#include <limits>

namespace lux::render
{
namespace
{
std::atomic<std::uint64_t> next_runtime{1}, next_view{1};
std::uint64_t issue(std::atomic<std::uint64_t> &sequence) noexcept
{
    auto value = sequence.load(std::memory_order_relaxed);
    while (value != (std::numeric_limits<std::uint64_t>::max)())
    {
        if (sequence.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
        {
            return value;
        }
    }
    return 0;
}
auto fail(ERendererError code) noexcept
{
    return lux::cxx::unexpected(RendererFailure{code});
}

template <class T, std::size_t Slots> bool clearJoinedRing(lux::cxx::BoundedSpscFrameRing<T, Slots> &ring) noexcept
{
    // Backend join transfers both ring roles here, including historical
    // storage slots that are not part of the published queue anymore.
    ring.currentRead().clear_keep_capacity();
    while (ring.tryAcquireRead())
    {
        ring.currentRead().clear_keep_capacity();
    }
    for (std::size_t i = 0; i < Slots; ++i)
    {
        auto *slot = ring.tryBeginWrite();
        if (!slot)
        {
            return false;
        }
        slot->clear_keep_capacity();
        if (!ring.publishWrite() || !ring.tryAcquireRead())
        {
            return false;
        }
    }
    return true;
}
} // namespace

struct RenderRuntime::Impl final
{
    explicit Impl(RendererConfig value)
        : config(std::move(value)), thread(config), views(config.view_capacity), diagnostics(config.diagnostic_capacity)
    {
    }

    const std::thread::id owner{std::this_thread::get_id()};
    RendererConfig config;
    detail::RendererThread thread;
    RenderProgramSession programs{thread.frames, thread.sync};
    RenderControlSession control{thread.controls, thread.sync};
    RenderUploadSession uploads{thread.uploads, thread.sync};
    std::shared_ptr<detail::UploadQueue> upload_queue{
        std::make_shared<detail::UploadQueue>(config.upload_capacity, config.upload_byte_capacity)};
    RenderUploadClient upload_client{RenderUploadClient::bind(upload_queue, &detail::UploadQueue::submit)};
    std::jthread worker;
    const std::uint64_t identity{issue(next_runtime)};
    std::vector<std::shared_ptr<detail::ViewResources>> views;
    std::vector<RendererDiagnostic> diagnostics;
    std::size_t head{}, count{}, next_reply_lane{}, storage_rotations{};
    std::optional<RendererDiagnostic> terminal_diagnostic;
    bool terminal_reported{}, closing{}, retired{}, joined{true}, busy{};

    struct FeatureBatch final
    {
        std::vector<RenderFeatureRegistration> candidates;
        std::vector<FeatureTypeRegisteredReply> accepted;
        RenderRequest<FeatureTypeRegisteredReply> registration;
        RenderRequest<GenericOkReply> rollback;
        FeatureRegistrationStatus status{EFeatureRegistrationState::REGISTERING, {}};
        bool cancelled{};
    };
    std::optional<FeatureBatch> feature_batch;

    [[nodiscard]] bool featureBatchPending() const noexcept
    {
        if (!feature_batch) return false;
        const auto state = feature_batch->status.state;
        return state == EFeatureRegistrationState::REGISTERING || state == EFeatureRegistrationState::READY ||
            state == EFeatureRegistrationState::ROLLING_BACK;
    }

    void advanceFeatureRegistration(std::size_t &budget)
    {
        if (!featureBatchPending()) return;
        auto &batch = *feature_batch;
        if (retired)
        {
            const auto error = thread.sync->terminalError();
            batch.status = {EFeatureRegistrationState::FAILED,
                error.ok() ? renderError<err::feature::InvalidRegistration>() : error};
            return;
        }
        if (batch.registration.valid())
        {
            if (!batch.registration.isReady()) return;
            auto value = batch.registration.tryResult();
            const auto error = value ? value->get().error : value.error();
            if (!error.ok() || (value && value->get().feature_type_id == 0))
            {
                batch.status.error = error.ok() ? renderError<err::feature::InvalidRegistration>() : error;
                batch.status.state = EFeatureRegistrationState::ROLLING_BACK;
            }
            else
            {
                batch.accepted.push_back(value->get());
            }
            batch.registration = {};
        }
        if (batch.status.state == EFeatureRegistrationState::ROLLING_BACK)
        {
            if (batch.rollback.valid())
            {
                if (!batch.rollback.isReady()) return;
                auto value = batch.rollback.tryResult();
                if (!value || value->get().code != 0 || !value->get().error.ok())
                {
                    const auto error = value ? value->get().error : value.error();
                    batch.status.error = error.ok() ? renderError<err::feature::InvalidRegistration>() : error;
                    // A rejected rollback is a backend contract failure; close must still
                    // retire the server before any candidate code can be released.
                    thread.sync->requestStop();
                    return;
                }
                batch.accepted.pop_back();
                batch.rollback = {};
            }
            if (batch.accepted.empty())
            {
                batch.status.state = batch.cancelled ? EFeatureRegistrationState::CANCELLED :
                                                       EFeatureRegistrationState::FAILED;
                return;
            }
            if (budget && control.canSubmit())
            {
                batch.rollback = control.unregisterFeatureType(batch.accepted.back().feature_type_id);
                --budget;
            }
            return;
        }
        if (batch.status.state != EFeatureRegistrationState::REGISTERING) return;
        if (batch.accepted.size() == batch.candidates.size())
        {
            batch.status.state = EFeatureRegistrationState::READY;
            return;
        }
        if (budget && control.canSubmit())
        {
            const auto &candidate = batch.candidates[batch.accepted.size()];
            batch.registration = control.registerFeatureType(candidate.factory, candidate.code_lifetime);
            --budget;
        }
    }

    RenderResult<void> check() const noexcept
    {
        if (owner != std::this_thread::get_id())
        {
            return fail(ERendererError::WRONG_THREAD);
        }
        if (busy)
        {
            return fail(ERendererError::BUSY);
        }
        return {};
    }

    void record(RendererDiagnostic value) noexcept
    {
        if (value.terminal)
        {
            terminal_diagnostic = value;
        }
        else if (count == diagnostics.size())
        {
            thread.statistics->dropped += value.occurrences;
        }
        else
        {
            diagnostics[(head + count++) % diagnostics.size()] = value;
        }
    }
};

RenderRuntime::RenderRuntime(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
{
}

RenderRuntime::~RenderRuntime()
{
    if (!impl_->joined)
    {
        // The composition root must finish asynchronous shutdown before
        // destroying its window, endpoint owners or feature code modules.
        std::terminate();
    }
}

RenderResult<std::unique_ptr<RenderRuntime>> RenderRuntime::create(RendererConfig config, ValidationMessageSink diagnostics)
{
    if (config.frame_capacity < 2 || config.frame_capacity > 3 || config.control_capacity < 2 ||
        config.control_capacity > 65536 || config.upload_capacity < 2 || config.upload_capacity > 65536 ||
        !config.upload_byte_capacity || !config.diagnostic_capacity || !config.scene_capacity || !config.view_capacity)
    {
        return fail(ERendererError::INVALID_ARGUMENT);
    }
    auto impl = std::make_unique<Impl>(std::move(config));
    if (!impl->identity)
    {
        return fail(ERendererError::CAPACITY);
    }
    impl->programs.setErrorEventHandler(
        [stats = impl->thread.statistics](const auto &batch) { stats->dropped += batch.dropped; },
        [owner = impl.get()](const auto &event) {
            owner->thread.statistics->events += event.occurrences;
            owner->record({{ERendererError::DEVICE_FAILURE, event.error},
                           event.scene_index,
                           event.occurrences,
                           event.frame_serial,
                           event.seq,
                           false});
        });
    auto result = std::unique_ptr<RenderRuntime>(new RenderRuntime(std::move(impl)));
    auto &data = *result->impl_;
    auto started = detail::startRendererThread(data.thread, data.config, std::move(diagnostics));
    if (!started)
    {
        return lux::cxx::unexpected(started.error());
    }
    data.worker = std::move(*started);
    while (data.thread.startup.load(std::memory_order_acquire) == 0)
    {
        data.thread.startup.wait(0, std::memory_order_acquire);
    }
    if (data.thread.startup.load(std::memory_order_acquire) != 1)
    {
        data.worker.join();
        return lux::cxx::unexpected(RendererFailure{ERendererError::DEVICE_FAILURE, data.thread.startup_error});
    }
    data.joined = false;
    return result;
}

RenderResult<RenderSceneLease> RenderRuntime::createScene(const RenderControlSession::CreateSceneConfig &config,
                                                          const std::vector<SceneFeatureAttachment> &features)
{
    if (auto checked = impl_->check(); !checked)
    {
        return lux::cxx::unexpected(checked.error());
    }
    if (impl_->closing || impl_->thread.sync->isStopping())
    {
        return fail(ERendererError::STOPPING);
    }
    if (!config.name || !std::isfinite(config.coordinate_page_size) || config.coordinate_page_size <= 0)
    {
        return fail(ERendererError::INVALID_ARGUMENT);
    }
    if (impl_->control.sceneRecordCount() == impl_->config.scene_capacity)
    {
        return fail(ERendererError::CAPACITY);
    }
    return impl_->control.prepareScene(config, features);
}

RenderResult<std::unique_ptr<RenderView>> RenderRuntime::openView(const RenderSceneLease &scene, ViewConfig config)
{
    if (auto checked = impl_->check(); !checked)
    {
        return lux::cxx::unexpected(checked.error());
    }
    if (impl_->closing || impl_->thread.sync->isStopping())
    {
        return fail(ERendererError::STOPPING);
    }
    const auto *surface = std::get_if<NativeSurfaceOutput>(&config.output);
    if (!impl_->control.owns(scene) || (surface && !surface->native_window) || config.extent.width > 16384 ||
        config.extent.height > 16384)
    {
        return fail(ERendererError::INVALID_ARGUMENT);
    }
    if (scene.status().state != ESceneResourceState::READY)
    {
        return fail(ERendererError::NOT_READY);
    }
    const auto slot = std::find(impl_->views.begin(), impl_->views.end(), nullptr);
    if (slot == impl_->views.end())
    {
        return fail(ERendererError::CAPACITY);
    }
    const auto generation = issue(next_view);
    if (!generation)
    {
        return fail(ERendererError::CAPACITY);
    }
    auto use = scene.retain();
    if (!use)
    {
        return fail(ERendererError::STOPPING);
    }
    const RenderViewId id{impl_->identity, static_cast<std::uint64_t>(slot - impl_->views.begin()), generation};
    auto record = std::make_shared<detail::ViewResources>(*this, impl_->control, id, std::move(use), config);
    auto result = std::unique_ptr<RenderView>(new RenderView(record));
    *slot = std::move(record); // Before any asynchronous create request.
    return result;
}

RenderResult<ViewObservation> RenderRuntime::observeView(RenderViewId id) const noexcept
{
    if (auto checked = impl_->check(); !checked)
    {
        return lux::cxx::unexpected(checked.error());
    }
    if (id.renderer != impl_->identity || id.slot >= impl_->views.size())
    {
        return lux::cxx::unexpected(RendererFailure{ERendererError::STALE_VIEW, {}, id});
    }
    const auto &record = impl_->views[id.slot];
    if (!record || record->status.view != id)
    {
        return lux::cxx::unexpected(RendererFailure{ERendererError::STALE_VIEW, {}, id});
    }
    auto status = record->status;
    if (record->close_requested && status.state != EViewState::CLOSED)
    {
        status.state = EViewState::CLOSING;
    }
    return ViewObservation{record->scene, record->view, status};
}

bool RenderRuntime::controlAvailable(std::size_t packets) const noexcept
{
    if (impl_->owner != std::this_thread::get_id() || impl_->thread.sync->isStopping())
    {
        return false;
    }
    return impl_->control.canSubmit(packets);
}

RenderResult<ImageContentStamp> RenderRuntime::imageEvidence(const ViewImage &image) const noexcept
{
    if (auto checked = impl_->check(); !checked)
    {
        return lux::cxx::unexpected(checked.error());
    }
    const auto *record = detail::ViewImageAccess::record(image);
    if (!record || !record->version || image.view.renderer != impl_->identity)
    {
        return fail(ERendererError::STALE_IMAGE);
    }
    const auto &version = *record->version;
    if (image.view != version.id || image.extent != version.extent || image.texture != version.texture ||
        image.content.source != record->content.source || image.content.frame_serial != record->content.frame_serial ||
        image.content.evidence != record->content.evidence)
    {
        return fail(ERendererError::STALE_IMAGE);
    }
    auto evidence = record->content;
    if (const auto submitted = record->submitted.load(std::memory_order_acquire))
    {
        evidence.frame_serial = submitted;
        evidence.evidence = impl_->thread.statistics->completed.load(std::memory_order_acquire) >= submitted
                                ? EImageEvidence::GPU_COMPLETE
                                : EImageEvidence::RECORDED;
    }
    return evidence;
}

RenderResult<std::reference_wrapper<RenderControlSession>> RenderRuntime::control() noexcept
{
    if (auto checked = impl_->check(); !checked)
    {
        return lux::cxx::unexpected(checked.error());
    }
    return std::ref(impl_->control);
}

RenderResult<RenderUploadClient> RenderRuntime::upload() noexcept
{
    if (auto checked = impl_->check(); !checked)
    {
        return lux::cxx::unexpected(checked.error());
    }
    return impl_->upload_client;
}

const FeatureCatalog &RenderRuntime::features() const noexcept
{
    return impl_->thread.catalog;
}

RenderResult<EFrameSubmit> RenderRuntime::submit(RenderProgram<> &input) noexcept
{
    if (auto checked = impl_->check(); !checked)
    {
        return lux::cxx::unexpected(checked.error());
    }
    if (impl_->closing || impl_->thread.sync->isStopping())
    {
        return fail(ERendererError::STOPPING);
    }
    if (!impl_->programs.trySubmitPrepared(input))
    {
        return EFrameSubmit::BACKPRESSURED;
    }
    // Four storage slots retain attachments independently of FIFO capacity
    // and GPU FIF. Idle retirement rotates that storage a bounded one turn
    // at a time, without pretending these are business updates.
    impl_->storage_rotations = RenderProgramChannel<>::request_slot_count;
    return EFrameSubmit::SUBMITTED;
}

RenderResult<std::size_t> RenderRuntime::poll(std::size_t replies, std::size_t &controls, std::size_t &programs)
{
    auto &data = *impl_;
    if (auto checked = data.check(); !checked)
    {
        return lux::cxx::unexpected(checked.error());
    }
    struct Gate final
    {
        bool &busy;
        ~Gate()
        {
            busy = false;
        }
    } gate{data.busy};
    data.busy = true;
    const auto consumed =
        detail::pumpRendererReplies(data.control, data.programs, data.uploads, data.next_reply_lane, replies);
    const auto terminal = data.thread.sync->terminalError();
    if (!data.terminal_reported && !terminal.ok())
    {
        data.record({{ERendererError::DEVICE_FAILURE, terminal}, RenderErrorEvent::kNoScene, 1, 0, 0, true});
        data.terminal_reported = true;
    }
    if (data.thread.stopped.load(std::memory_order_acquire) && !data.retired)
    {
        data.worker.join();
        if (!clearJoinedRing(data.thread.frames->requests))
        {
            return fail(ERendererError::CONTRACT_FAILURE);
        }
        data.programs.rawClient().retireAfterBackendStopped();
        data.control.retireScenesAfterBackendStopped();
        data.retired = true;
    }
    data.advanceFeatureRegistration(controls);
    // Upload forwarding consumes the explicit command-work allowance;
    // accepted uploads are never counted as consumed reply envelopes.
    controls -= data.upload_queue->poll(data.uploads, data.thread.sync->isStopping(), controls);
    bool view_maintenance{};
    for (auto &view : data.views)
    {
        if (!view)
        {
            continue;
        }
        const auto advanced = view->prepareControlStep(controls);
        if (!advanced)
        {
            return lux::cxx::unexpected(advanced.error());
        }
        if (view->pending_failure)
        {
            data.record({*view->pending_failure});
            view->pending_failure.reset();
        }
        const auto state = view->status.state;
        view_maintenance |= state == EViewState::RESIZING || state == EViewState::CLOSING;
        if (state == EViewState::CLOSED)
        {
            view.reset();
        }
    }
    // Release callbacks distinguish stop intent from actual backend retirement.
    // Keep adopting their late results after the backend has joined.
    controls -= data.control.maintainResources(controls);
    if (!data.thread.sync->isStopping())
    {
        const bool releases = data.control.pendingSceneReleases() || data.control.pendingTargetReleases() ||
                              data.control.pendingViewReleases() || data.control.pendingResourceReleases();
        if (programs && (data.storage_rotations || releases || view_maintenance))
        {
            RenderProgram<> maintenance;
            RenderProgramSession::Builder builder(maintenance);
            builder.begin({});
            maintenance.kind = ERenderProgramKind::StateUpdate;
            if (data.programs.trySubmitPrepared(maintenance))
            {
                --programs;
                if (data.storage_rotations)
                {
                    --data.storage_rotations;
                }
            }
        }
    }
    return consumed;
}

RenderResult<void> RenderRuntime::beginFeatureRegistration(std::vector<RenderFeatureRegistration> candidates)
{
    if (auto checked = impl_->check(); !checked) return checked;
    if (impl_->closing || impl_->thread.sync->isStopping()) return fail(ERendererError::STOPPING);
    if (impl_->featureBatchPending()) return fail(ERendererError::BUSY);
    std::vector<FeatureTypeId> identities;
    std::vector<std::string_view> names;
    for (const auto &candidate : candidates)
    {
        const auto &factory = candidate.factory;
        const auto &descriptor = factory.descriptor;
        const bool invalid_identity = !descriptor.valid() || descriptor.canonical_name.empty() ||
            featureId(descriptor.canonical_name) != descriptor.type || !descriptor.abi_version;
        const bool invalid_factory = !factory.create_fn || !factory.name || factory.name[0] == '\0' ||
            factory.operation_count > 16 ||
            (factory.operation_count && (!factory.register_ops_fn || !factory.unregister_ops_fn)) ||
            (candidate.scene_configurable && !candidate.configuration.valid());
        if (invalid_identity || invalid_factory) return fail(ERendererError::INVALID_ARGUMENT);
        if (impl_->thread.catalog.find(descriptor.type) || impl_->thread.catalog.find(factory.name) ||
            std::ranges::find(identities, descriptor.type) != identities.end() ||
            std::ranges::find(names, factory.name) != names.end()) return fail(ERendererError::INVALID_ARGUMENT);
        identities.push_back(descriptor.type);
        names.push_back(factory.name);
    }
    impl_->feature_batch.emplace();
    impl_->feature_batch->candidates = std::move(candidates);
    impl_->feature_batch->accepted.reserve(impl_->feature_batch->candidates.size());
    return {};
}

FeatureRegistrationStatus RenderRuntime::featureRegistrationStatus() const noexcept
{
    return impl_->feature_batch ? impl_->feature_batch->status : FeatureRegistrationStatus{};
}

RenderResult<void> RenderRuntime::commitFeatureRegistration()
{
    if (auto checked = impl_->check(); !checked) return checked;
    if (!impl_->feature_batch || impl_->feature_batch->status.state != EFeatureRegistrationState::READY)
        return fail(ERendererError::NOT_READY);
    auto &batch = *impl_->feature_batch;
    for (std::size_t index = 0; index < batch.candidates.size(); ++index)
    {
        const auto &reply = batch.accepted[index];
        auto inserted = impl_->thread.catalog.add(batch.candidates[index], reply.feature_type_id,
                                                  {reply.ops, reply.op_count});
        // Main validated the complete input before registration; no other writer
        // can publish between begin and commit. Allocation failure terminates.
        if (!inserted) std::terminate();
    }
    batch.status.state = EFeatureRegistrationState::COMMITTED;
    return {};
}

RenderResult<void> RenderRuntime::cancelFeatureRegistration() noexcept
{
    if (auto checked = impl_->check(); !checked) return checked;
    if (impl_->featureBatchPending())
    {
        impl_->feature_batch->cancelled = true;
        impl_->feature_batch->status.state = EFeatureRegistrationState::ROLLING_BACK;
    }
    return {};
}

RenderRuntimeStatus RenderRuntime::status() const noexcept
{
    return {impl_->retired                     ? ERenderRuntimeState::RETIRED
            : impl_->thread.sync->isStopping() ? ERenderRuntimeState::STOPPING
                                               : ERenderRuntimeState::ACTIVE,
            impl_->thread.sync->terminalError()};
}

RendererStatistics RenderRuntime::statistics() const noexcept
{
    const auto &stats = *impl_->thread.statistics;
    return {stats.frames.load(),
            stats.events.load(),
            stats.dropped.load(),
            stats.completed.load(),
            impl_->control.activeResourceUses(),
            static_cast<std::size_t>(
                std::count_if(impl_->views.begin(), impl_->views.end(), [](const auto &v) { return bool(v); })),
            impl_->thread.frames->requests.pendingFrames(),
            static_cast<std::uint64_t>(stats.validation_errors.load())};
}

RenderResult<std::optional<RendererDiagnostic>> RenderRuntime::takeDiagnostic() noexcept
{
    if (auto checked = impl_->check(); !checked)
    {
        return lux::cxx::unexpected(checked.error());
    }
    if (!impl_->count)
    {
        return std::exchange(impl_->terminal_diagnostic, std::nullopt);
    }
    const auto result = impl_->diagnostics[impl_->head];
    impl_->head = (impl_->head + 1) % impl_->diagnostics.size();
    --impl_->count;
    return std::optional{result};
}

RenderResult<void> RenderRuntime::beginClose() noexcept
{
    if (auto checked = impl_->check(); !checked)
    {
        return checked;
    }
    auto cancelled = cancelFeatureRegistration();
    if (!cancelled) return cancelled;
    impl_->closing = true;
    impl_->upload_queue->stop();
    return {};
}

RenderResult<ERenderClose> RenderRuntime::advanceClose(std::size_t &replies, std::size_t &controls,
                                                       std::size_t &programs)
{
    if (!impl_->closing)
    {
        return fail(ERendererError::BUSY);
    }
    const auto adopted = poll(replies, controls, programs);
    if (!adopted)
    {
        return lux::cxx::unexpected(adopted.error());
    }
    replies -= *adopted;
    if (impl_->featureBatchPending() || impl_->control.activeResourceUses() || impl_->control.pendingResourceReleases() ||
        !impl_->upload_queue->empty())
    {
        return ERenderClose::PENDING;
    }
    if (impl_->retired)
    {
        return ERenderClose::COMPLETE;
    }
    if (impl_->storage_rotations || impl_->control.pendingSceneReleases() || impl_->control.pendingViewReleases() ||
        impl_->control.pendingTargetReleases())
    {
        return ERenderClose::PENDING;
    }
    impl_->thread.sync->requestStop();
    return ERenderClose::PENDING;
}

RenderResult<void> RenderRuntime::joinStopped() noexcept
{
    if (auto checked = impl_->check(); !checked)
    {
        return checked;
    }
    if (!impl_->retired || impl_->control.activeResourceUses() || !impl_->upload_queue->empty())
    {
        return fail(ERendererError::BUSY);
    }
    impl_->joined = true;
    return {};
}
} // namespace lux::render
