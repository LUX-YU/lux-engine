#include <lux/engine/scene/detail/RenderResourcesImpl.hpp>

#include <algorithm>
#include <limits>

namespace lux::scene
{
    namespace
    {
        auto fail(render::ERendererError code) noexcept
        {
            return lux::cxx::unexpected(render::RendererFailure{code});
        }
        void failView(
            detail::ViewResult& result,
            render::RenderError error,
            std::uint64_t request,
            std::optional<std::uint32_t> backend = {}
        ) noexcept
        {
            auto& status = result.value.status;
            if (!status.failure)
                status.failure = render::RendererFailure{
                    error.ok() && !backend ? render::ERendererError::CONTRACT_FAILURE
                                           : render::ERendererError::DEVICE_FAILURE,
                    error,
                    request,
                    backend
                };
            if (status.state != EViewState::CLOSING && status.state != EViewState::CLOSED)
                status.state = EViewState::FAILED;
        }
    }

    ViewObservation RenderViewReceipt::status() const noexcept
    {
        if (record_)
            return record_->value;
        ViewObservation result;
        result.status.state = EViewState::CLOSED;
        return result;
    }

    render::RenderResult<RenderResourceId> RenderResources::requestView(
        RenderResourceId source,
        ViewConfig config
    ) noexcept
    {
        auto& self = *impl_;
        self.requireOwner();
        if (self.closing || self.runtime.status().state != render::ERenderRuntimeState::ACTIVE)
            return fail(render::ERendererError::STOPPING);
        auto* parent = self.find(source);
        auto* scene = parent ? std::get_if<detail::SceneState>(&parent->payload) : nullptr;
        const auto* surface = std::get_if<NativeSurfaceOutput>(&config.output);
        const bool invalid_extent = config.extent.width > 16384 || config.extent.height > 16384;
        if (!scene || (surface && !surface->native_window) || invalid_extent)
            return fail(render::ERendererError::INVALID_ARGUMENT);
        if (scene->status->state != ESceneResourceState::READY || !scene->status->failure.ok())
            return fail(render::ERendererError::NOT_READY);
        if (self.records.size() == self.limits.requests ||
            parent->references == std::numeric_limits<std::size_t>::max())
            return fail(render::ERendererError::CAPACITY);
        {
            auto record = std::make_unique<Impl::Record>();
            auto& view = record->payload.emplace<detail::EViewState>();
            view.result = std::make_shared<detail::ViewResult>();
            view.result->value.scene = scene->status->scene;
            view.result->value.status = {EViewState::CREATING, config.extent, {}, 1, 0};
            view.scene = source;
            view.output = config.output;
            const auto key = self.records.emplace(std::move(record));
            auto& admitted = *self.records[key];
            admitted.id = {self.domain, key};
            ++parent->references;
            self.enqueue(admitted);
            return admitted.id;
        }
    }

    render::RenderResult<ViewObservation> RenderResources::observeView(RenderResourceId id) const noexcept
    {
        const auto* record = impl_->find(id);
        const auto* view = record ? std::get_if<detail::EViewState>(&record->payload) : nullptr;
        if (!view)
            return fail(render::ERendererError::STALE_VIEW);
        return view->result->value;
    }
    render::RenderResult<RenderViewReceipt> RenderResources::viewReceipt(RenderResourceId id) const noexcept
    {
        const auto* record = impl_->find(id);
        const auto* view = record ? std::get_if<detail::EViewState>(&record->payload) : nullptr;
        if (!view)
            return fail(render::ERendererError::STALE_VIEW);
        RenderViewReceipt receipt;
        receipt.record_ = view->result;
        return receipt;
    }
    render::RenderResult<void> RenderResources::requestViewExtent(
        RenderResourceId id,
        render::PixelExtent extent
    ) noexcept
    {
        auto* record = impl_->find(id);
        auto* view = record ? std::get_if<detail::EViewState>(&record->payload) : nullptr;
        if (!view)
            return fail(render::ERendererError::STALE_VIEW);
        auto& status = view->result->value.status;
        if (status.failure)
            return lux::cxx::unexpected(*status.failure);
        if (extent.width > 16384 || extent.height > 16384)
            return fail(render::ERendererError::INVALID_ARGUMENT);
        if (extent == status.requested_extent)
            return {};
        if (status.request_sequence == UINT64_MAX)
            return fail(render::ERendererError::CAPACITY);
        status.requested_extent = extent;
        ++status.request_sequence;
        status.state = extent.width && extent.height ? EViewState::RESIZING : EViewState::SUSPENDED;
        impl_->enqueue(*record);
        return {};
    }
    render::RenderResult<void> RenderResources::setViewOutput(RenderResourceId id, ViewStamp stamp) noexcept
    {
        auto* record = impl_->find(id);
        auto* view = record ? std::get_if<detail::EViewState>(&record->payload) : nullptr;
        if (!view)
            return fail(render::ERendererError::STALE_VIEW);
        if (view->result->value.status.failure)
            return lux::cxx::unexpected(*view->result->value.status.failure);
        view->desired = stamp;
        // Publication may precede adoption of the newly produced output. Stamp its exact
        // generation whether it is pending or current; an older frozen image keeps its identity.
        for (const auto id : {view->current, view->pending})
        {
            if (auto* output = impl_->find(id))
            {
                auto& state = std::get<detail::OutputState>(output->payload);
                if (state.generation == stamp.surface_generation)
                    state.info.content.source = stamp;
            }
        }
        return {};
    }
    render::RenderResult<RenderResourceId> RenderResources::viewOutput(RenderResourceId id) const noexcept
    {
        const auto* record = impl_->find(id);
        const auto* view = record ? std::get_if<detail::EViewState>(&record->payload) : nullptr;
        if (!view)
            return fail(render::ERendererError::STALE_VIEW);
        if (!view->sampled())
            return fail(render::ERendererError::INVALID_ARGUMENT);
        const auto& status = view->result->value.status;
        if (status.failure && !view->current.isValid())
            return lux::cxx::unexpected(*status.failure);
        if (!view->current.isValid() || status.state == EViewState::SUSPENDED)
            return fail(render::ERendererError::NOT_READY);
        return view->current;
    }
    render::RenderResult<RenderOutputInfo> RenderResources::outputInfo(RenderResourceId id) const noexcept
    {
        const auto* record = impl_->find(id);
        const auto* output = record ? std::get_if<detail::OutputState>(&record->payload) : nullptr;
        if (!output)
            return fail(render::ERendererError::INVALID_ARGUMENT);
        if (!output->info.target.isValid())
            return fail(render::ERendererError::NOT_READY);
        auto result = output->info;
        if (auto submitted = output->produced.submitted())
        {
            result.content.frame_serial = submitted;
            result.content.evidence =
                output->produced.completed() >= submitted ? EImageEvidence::GPU_COMPLETE : EImageEvidence::SUBMITTED;
        }
        else if (auto recorded = output->produced.recorded())
        {
            result.content.frame_serial = recorded;
            result.content.evidence = EImageEvidence::RECORDED;
        }
        return result;
    }

    render::RenderResult<RenderResourceId> RenderResources::Impl::makeOutput(detail::EViewState& view) noexcept
    {
        if (records.size() == limits.requests)
            return fail(render::ERendererError::CAPACITY);
        {
            auto record = std::make_unique<Record>();
            auto& output = record->payload.emplace<detail::OutputState>();
            output.owner = view.result;
            output.info.extent = view.result->value.status.requested_extent;
            output.generation = view.result->value.status.request_sequence;
            output.sampled = view.sampled();
            if (view.desired.surface_generation == output.generation)
                output.info.content.source = view.desired;
            output.info.content.source.surface_generation = output.generation;
            if (output.sampled)
            {
                detail::TextureRecords reserved;
                reserved.emplace(render::RTextureHandle{}, nullptr);
                output.lookup = reserved.extract(reserved.begin());
                auto production = render::RenderSubmissionState::create(completion, &Impl::wake);
                if (!production)
                    return fail(render::ERendererError::CAPACITY);
                output.production = std::move(*production);
                output.produced = output.production.observe();
            }
            else
                output.native_window = std::get<NativeSurfaceOutput>(view.output).native_window;
            const auto key = records.emplace(std::move(record));
            auto& admitted = *records[key];
            admitted.id = {domain, key};
            ++view.result->outputs;
            enqueue(admitted);
            return admitted.id;
        }
    }

    void RenderResources::Impl::advanceOutput(Record& record) noexcept
    {
        auto& output = std::get<detail::OutputState>(record.payload);
        if (output.retired)
            return;
        const auto backend = runtime.status();
        if (backend.state == render::ERenderRuntimeState::RETIRED)
        {
            output.create = {};
            output.release = {};
            output.production = {};
            output.retired = true;
            return;
        }
        if (output.create.valid() && output.create.isReady())
        {
            auto result = output.create.tryResult();
            const bool valid = result && result->get().status == 0 && result->get().target.isValid() &&
                               (!output.sampled || result->get().texture.isValid());
            // Even a malformed successful reply must not lose its real target.
            if (result)
            {
                output.info.target = result->get().target;
                output.info.texture = result->get().texture;
                output.info.extent = {result->get().extent.width, result->get().extent.height};
            }
            if (!valid)
                failView(
                    *output.owner,
                    result ? render::RenderError{} : result.error(),
                    output.create.requestId(),
                    result ? std::optional{result->get().status} : std::nullopt
                );
            else if (output.sampled && record.references)
            {
                output.lookup.key() = output.info.texture;
                output.lookup.mapped() = &record;
                texture_records.insert(std::move(output.lookup));
            }
            output.create = {};
        }
        if (output.release.valid() && output.release.isReady())
        {
            auto result = output.release.tryResult();
            if (result && result->get().status <= 1 && result->get().target == output.info.target)
                output.retired = true;
            else
            {
                failView(
                    *output.owner,
                    result ? render::RenderError{} : result.error(),
                    output.release.requestId(),
                    result ? std::optional{result->get().status} : std::nullopt
                );
                runtime.control()->get().requestStop();
            }
            output.release = {};
        }
        if (output.retired)
            return;
        if (backend.state != render::ERenderRuntimeState::ACTIVE)
            return;
        if (!record.references)
        {
            std::erase_if(record.submissions, [](const auto& use) { return use.complete(); });
            if (output.create.valid() || output.release.valid() || !record.submissions.empty())
                return;
            if (!output.info.target.isValid())
            {
                output.retired = true;
                return;
            }
            if (runtime.controlAvailable())
                output.release = runtime.control()->get().destroyRenderTarget(output.info.target);
            return;
        }
        if (!output.admitted && runtime.controlAvailable())
        {
            const auto extent = output.info.extent;
            output.create = output.sampled ? runtime.control()->get().createOffscreenRenderTarget(
                                                 {extent.width, extent.height},
                                                 render::kTargetFlagSampled
                                             )
                                           : runtime.control()->get().createSurfaceRenderTarget(
                                                 output.native_window,
                                                 {extent.width, extent.height}
                                             );
            output.admitted = true;
        }
    }

    void RenderResources::Impl::advanceView(Record& record) noexcept
    {
        auto& view = std::get<detail::EViewState>(record.payload);
        auto& observation = view.result->value;
        auto& status = observation.status;
        const bool closing = !record.references;
        if (status.state == EViewState::CLOSED)
            return;
        if (closing)
            status.state = EViewState::CLOSING;
        const auto drop = [&](RenderResourceId& id) {
            if (id.isValid())
                release(std::exchange(id, {}));
        };
        const auto output = [&](RenderResourceId id) -> detail::OutputState* {
            auto* entry = find(id);
            return entry ? std::get_if<detail::OutputState>(&entry->payload) : nullptr;
        };
        const auto backend = runtime.status();
        if (backend.state != render::ERenderRuntimeState::ACTIVE)
        {
            failView(
                *view.result,
                backend.error.ok() ? render::renderError<render::err::comm::ChannelStopping>() : backend.error,
                0
            );
            if (backend.state == render::ERenderRuntimeState::RETIRED && closing)
            {
                drop(view.scene);
                drop(view.current);
                drop(view.pending);
                view.create = {};
                view.switch_target = {};
                view.release = {};
                view.resize = {};
                observation.handle = {};
                observation.render_extent = {};
                observation.render_sequence = 0;
                if (!view.result->outputs)
                    status.state = EViewState::CLOSED;
            }
            return;
        }
        if (view.create.valid() && view.create.isReady())
        {
            auto result = view.create.tryResult();
            if (result && result->get().error.ok() && result->get().view.isValid())
                observation.handle = result->get().view;
            else
                failView(*view.result, result ? result->get().error : result.error(), view.create.requestId());
            view.create = {};
        }
        if (view.switch_target.valid() && view.switch_target.isReady())
        {
            auto result = view.switch_target.tryResult();
            if (result && result->get().code == 0 && result->get().error.ok())
            {
                view.producer = view.switch_next;
                auto* next = output(view.pending.isValid() ? view.pending : view.current);
                observation.render_extent = view.producer.isValid() && next ? next->info.extent : render::PixelExtent{};
                observation.render_sequence = view.producer.isValid() && next ? next->generation : 0;
                if (next && view.producer == next->info.target)
                    next->production = {};
            }
            else
                failView(
                    *view.result,
                    result ? result->get().error : result.error(),
                    view.switch_target.requestId(),
                    result ? std::optional{result->get().code} : std::nullopt
                );
            view.switch_target = {};
        }
        if (view.resize.valid() && view.resize.isReady())
        {
            auto result = view.resize.tryResult();
            auto* current = output(view.current);
            if (result && result->get().status == 0 && current && result->get().target == current->info.target)
            {
                current->info.extent = {result->get().extent.width, result->get().extent.height};
                current->generation = view.resize_sequence;
                observation.render_extent = status.ready_extent = current->info.extent;
                status.acknowledged_sequence = view.resize_sequence;
                observation.render_sequence = view.resize_sequence;
            }
            else
                failView(
                    *view.result,
                    result ? render::RenderError{} : result.error(),
                    view.resize.requestId(),
                    result ? std::optional{result->get().status} : std::nullopt
                );
            view.resize = {};
        }
        if (view.release.valid() && view.release.isReady())
        {
            auto result = view.release.tryResult();
            if (result && result->get().code == 0 && result->get().error.ok())
            {
                observation.handle = {};
                drop(view.scene); // Frozen outputs retain no Scene or Feature resources.
            }
            else
            {
                failView(*view.result, result ? result->get().error : result.error(), view.release.requestId());
                runtime.control()->get().requestStop();
            }
            view.release = {};
        }
        auto* pending = output(view.pending);
        if (!closing && !status.failure && pending && !view.switch_target.valid() && pending->info.target.isValid() &&
            view.producer == pending->info.target && (!view.sampled() || pending->produced.submitted()))
        {
            drop(view.current);
            view.current = std::exchange(view.pending, {});
            status.ready_extent = pending->info.extent;
            status.acknowledged_sequence = pending->generation;
            pending = nullptr;
        }
        if (view.switch_target.valid() || view.resize.valid())
            return;
        const auto desired = status.requested_extent;
        const bool suspended = !desired.width || !desired.height;
        if (closing || suspended)
        {
            // Accepted or locally prepared packets can still refer to this ViewHandle.
            if (!record.submissions.empty())
                return;
            if (view.producer.isValid())
            {
                if (!runtime.controlAvailable())
                    return;
                view.switch_next = {};
                if (view.sampled())
                    view.switch_target = runtime.control()->get().switchViewTarget(
                        view.producer,
                        {},
                        observation.scene,
                        observation.handle
                    );
                else
                {
                    runtime.control()->get().removeLayer(view.producer, 0);
                    view.producer = {};
                    observation.render_extent = {};
                    observation.render_sequence = 0;
                }
                return;
            }
            if (view.create.valid())
                return;
            drop(view.pending);
            if (closing)
            {
                drop(view.current);
                if (observation.handle.isValid())
                {
                    if (!view.release.valid() && runtime.controlAvailable())
                        view.release = runtime.control()->get().removeView(observation.scene, observation.handle);
                }
                else
                {
                    drop(view.scene);
                    if (!view.result->outputs)
                        status.state = EViewState::CLOSED;
                }
            }
            else
                status.state = EViewState::SUSPENDED;
            return;
        }
        if (status.failure)
        {
            if (pending && !pending->create.valid() &&
                (!pending->info.target.isValid() || pending->info.target != view.producer))
                drop(view.pending);
            return;
        }
        if (!observation.handle.isValid())
        {
            if (!view.create.valid() && runtime.controlAvailable())
                view.create =
                    runtime.control()->get().addView(observation.scene, {desired.width, desired.height}, "Scene view");
            return;
        }
        // A sampled producer may never receive a draw (hidden/paused UI). A newer extent
        // must not wait for an obsolete, unproduced output to become the current image.
        if (pending && pending->generation != status.request_sequence && !pending->create.valid())
        {
            if (view.producer == pending->info.target && view.producer.isValid())
            {
                if (!runtime.controlAvailable())
                    return;
                view.switch_next = {};
                view.switch_target =
                    runtime.control()->get().switchViewTarget(view.producer, {}, observation.scene, observation.handle);
                return;
            }
            drop(view.pending);
            pending = nullptr;
        }
        if (pending)
        {
            if (!pending->info.target.isValid() || pending->create.valid())
                return;
            if (pending->info.target != view.producer)
            {
                if (pending->generation != status.request_sequence)
                {
                    drop(view.pending);
                    return;
                }
                if (!runtime.controlAvailable())
                    return;
                view.switch_next = pending->info.target;
                view.switch_target =
                    view.sampled()
                        ? runtime.control()->get().switchViewTarget(
                              view.producer,
                              view.switch_next,
                              observation.scene,
                              observation.handle,
                              pending->production
                          )
                        : runtime.control()->get().setLayer(view.switch_next, 0, observation.scene, observation.handle);
            }
            status.state = view.current.isValid()
                               ? EViewState::RESIZING
                               : (view.producer == pending->info.target ? EViewState::READY : EViewState::CREATING);
            return;
        }
        auto* current = output(view.current);
        if (current && view.producer.isValid() && current->generation == status.request_sequence)
        {
            status.acknowledged_sequence = status.request_sequence;
            status.state = EViewState::READY;
            return;
        }
        if (current && !view.sampled())
        {
            if (!runtime.controlAvailable())
                return;
            if (!view.producer.isValid())
            {
                view.switch_next = current->info.target;
                view.switch_target =
                    runtime.control()->get().setLayer(view.switch_next, 0, observation.scene, observation.handle);
            }
            else
            {
                view.resize_sequence = status.request_sequence;
                view.resize =
                    runtime.control()->get().requestResizeTarget(current->info.target, {desired.width, desired.height});
            }
            return;
        }
        auto made = makeOutput(view);
        if (!made)
        {
            status.failure = made.error();
            status.state = EViewState::FAILED;
            return;
        }
        view.pending = *made;
        status.state = current ? EViewState::RESIZING : EViewState::CREATING;
    }
} // namespace lux::scene
