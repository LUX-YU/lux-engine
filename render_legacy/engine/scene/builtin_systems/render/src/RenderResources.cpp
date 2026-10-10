#include <lux/engine/scene/detail/RenderResourcesImpl.hpp>

#include <algorithm>
#include <limits>

namespace lux::scene
{
    namespace
    {
        using namespace detail;
        render::RendererFailure failure(render::ERendererError code) noexcept
        {
            return {code};
        }
        bool terminal(ERenderAssetState state) noexcept
        {
            return state == ERenderAssetState::FAILED || state == ERenderAssetState::CANCELLED ||
                   state == ERenderAssetState::UNREFERENCED;
        }
        std::uint64_t nextDomain() noexcept
        {
            static std::atomic<std::uint64_t> next{1};
            auto value = next.load(std::memory_order_relaxed);
            while (value != UINT64_MAX)
                if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
                    return value;
            return 0;
        }
    } // namespace

    void RenderResources::Impl::requireOwner() const noexcept
    {
        if (std::this_thread::get_id() != owner)
            render::renderFatal("RenderResources used outside its owner thread");
    }
    RenderResources::Impl::Record* RenderResources::Impl::find(RenderResourceId id, bool referenced) const noexcept
    {
        requireOwner();
        const auto* entry = id.domain == domain ? records.find(id.slot) : nullptr;
        if (!entry || (referenced && !(*entry)->references))
            return nullptr;
        return entry->get();
    }
    void RenderResources::Impl::enqueue(Record& record, bool wake) noexcept
    {
        if (!std::exchange(record.queued, true))
            active.push_back(record.id.slot);
        if (wake)
            work.request();
    }

    render::RenderResult<RenderResourceId> RenderResources::Impl::request(
        const RenderAssetInput& input,
        asset::AssetId asset,
        detail::EResourceKind kind,
        bool retry_failed
    ) noexcept
    {
        requireOwner();
        if (closing)
            return lux::cxx::unexpected(failure(render::ERendererError::STOPPING));
        if (!input)
            return lux::cxx::unexpected(failure(render::ERendererError::INVALID_ARGUMENT));
        const detail::ResourceKey key{input.source, input.version, asset, kind};
        if (const auto found = cache.find(key); found != cache.end())
        {
            auto& record = *records[found->second];
            const bool retry_terminal = retry_failed && terminal(record.asset().status.state);
            if (!retry_terminal)
            {
                if (record.references == std::numeric_limits<std::size_t>::max())
                    return lux::cxx::unexpected(failure(render::ERendererError::CAPACITY));
                ++record.references;
                return record.id;
            }
        }
        if (records.size() == limits.requests)
            return lux::cxx::unexpected(failure(render::ERendererError::CAPACITY));
        auto control = runtime.control();
        if (!control)
            return lux::cxx::unexpected(control.error());
        {
            auto record = std::make_unique<Record>();
            record->asset().key = key;
            record->asset().input = input;
            record->asset().retry_failed = retry_failed;
            if (kind == detail::EResourceKind::MESH)
                std::get<detail::MeshState>(record->asset().payload).operations =
                    runtime.features().ops<render::MeshStackOperationIds>("StandardMeshStack");
            else if (kind == detail::EResourceKind::MATERIAL)
                record->asset().payload.emplace<detail::MaterialState>().operations =
                    runtime.features().ops<render::MaterialOperationIds>("StandardMaterial");
            else
            {
                auto& texture = record->asset().payload.emplace<detail::TextureState>();
                detail::TextureRecords reserved;
                reserved.emplace(render::RTextureHandle{}, nullptr);
                texture.lookup = reserved.extract(reserved.begin());
            }
            std::visit(
                [](auto& value) {
                    using Result = typename decltype(value.read)::element_type;
                    value.read = std::make_shared<Result>();
                },
                record->asset().payload
            );
            // The manager owns the only resource/retirement record. Cross-thread
            // consumers receive passive state, never a pointer to this record.
            const Key admitted_key = records.emplace(std::move(record));
            auto& admitted = *records[admitted_key];
            admitted.id = {domain, admitted_key};
            cache.insert_or_assign(key, admitted_key);
            enqueue(admitted);
            return admitted.id;
        }
    }

    void RenderResources::Impl::release(RenderResourceId id) noexcept
    {
        auto* record = find(id);
        if (!record)
            render::renderFatal("Unbalanced or foreign RenderResourceId release");
        if (--record->references)
            return;
        if (auto* output = std::get_if<detail::OutputState>(&record->payload))
            texture_records.erase(output->info.texture);
        if (auto* view = std::get_if<detail::EViewState>(&record->payload))
            view->result->value.status.state = lux::scene::EViewState::CLOSING;
        if (std::holds_alternative<detail::AssetState>(record->payload))
        {
            const auto& asset = record->asset();
            if (const auto* texture = std::get_if<detail::TextureState>(&asset.payload);
                texture && texture->handle.isValid())
                texture_records.erase(texture->handle);
            const auto found = cache.find(asset.key);
            if (found != cache.end() && found->second == id.slot)
                cache.erase(found);
        }
        // Re-requesting this key now creates a new generation; no resurrection of
        // an irreversible destroy or of an input abandoned by its last owner.
        enqueue(*record);
    }

    RenderResources::RenderResources(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
    RenderResources::CreateResult RenderResources::create(
        render::RenderRuntime& runtime,
        process::TaskScope& tasks,
        process::CpuScheduler cpu,
        RenderAssetLimits limits
    ) noexcept
    {
        if (!limits.requests || limits.requests >= UINT32_MAX)
            return lux::cxx::unexpected(failure(render::ERendererError::INVALID_ARGUMENT));
        const auto domain = nextDomain();
        if (!domain)
            return lux::cxx::unexpected(failure(render::ERendererError::CAPACITY));
        {
            auto impl = std::make_unique<Impl>(runtime, tasks, std::move(cpu), limits, domain);
            impl->records.reserve(limits.requests);
            impl->cache.reserve(limits.requests);
            impl->texture_records.reserve(limits.requests);
            impl->active.reserve(limits.requests);
            impl->batch.reserve(limits.requests);
            const auto connected = runtime.bindProgress(impl->completion, &Impl::wake);
            if (!connected)
                return lux::cxx::unexpected(connected.error());
            return std::unique_ptr<RenderResources>(new RenderResources(std::move(impl)));
        }
    }
    RenderResources::~RenderResources() noexcept
    {
        impl_->requireOwner();
        beginClose();
        // Consumers release business references first. GPU and read completions keep using the normal Main path.
        if (!impl_->tasks.execution().waitUntil([&]() noexcept { return impl_->records.empty(); }))
            render::renderFatal("RenderResources destruction cannot wait on this thread or callback");
        impl_->work.cancel();
    }

    render::RenderResult<RenderResourceId> RenderResources::requestMesh(
        const RenderAssetInput& input,
        asset::AssetId id,
        bool retry
    ) noexcept
    {
        return impl_->request(input, id, detail::EResourceKind::MESH, retry);
    }
    render::RenderResult<RenderResourceId> RenderResources::requestMaterial(
        const RenderAssetInput& input,
        asset::AssetId id,
        bool retry
    ) noexcept
    {
        return impl_->request(input, id, detail::EResourceKind::MATERIAL, retry);
    }
    render::RenderResult<RenderResourceId> RenderResources::requestTexture(
        const RenderAssetInput& input,
        asset::AssetId id,
        bool retry
    ) noexcept
    {
        return impl_->request(input, id, detail::EResourceKind::TEXTURE, retry);
    }
    render::RenderResult<void> RenderResources::retain(RenderResourceId id) noexcept
    {
        auto* record = impl_->find(id);
        if (!record)
            return lux::cxx::unexpected(failure(render::ERendererError::INVALID_ARGUMENT));
        if (record->references == std::numeric_limits<std::size_t>::max())
            return lux::cxx::unexpected(failure(render::ERendererError::CAPACITY));
        ++record->references;
        return {};
    }
    void RenderResources::release(RenderResourceId id) noexcept
    {
        impl_->release(id);
    }

    render::RenderResult<render::RenderSubmissionState> RenderResources::Impl::capture() noexcept
    {
        // Records have stable addresses until release retires them. This synchronous owner-thread
        // operation invokes no external callbacks and never leaves scratch borrowed by a submission.
        std::sort(capture_records.begin(), capture_records.end(), [](const Record* a, const Record* b) noexcept {
            return a->id.slot.index != b->id.slot.index ? a->id.slot.index < b->id.slot.index
                                                        : a->id.slot.gen < b->id.slot.gen;
        });
        capture_records.erase(std::unique(capture_records.begin(), capture_records.end()), capture_records.end());
        for (const auto* record : capture_records)
        {
            if (!record->references)
                return lux::cxx::unexpected(failure(render::ERendererError::INVALID_ARGUMENT));
            const auto* scene = std::get_if<detail::SceneState>(&record->payload);
            const auto* asset = std::get_if<detail::AssetState>(&record->payload);
            const auto* output = std::get_if<detail::OutputState>(&record->payload);
            const auto* view = std::get_if<detail::EViewState>(&record->payload);
            const bool view_ready = view && view->result->value.handle.isValid() &&
                                    view->result->value.status.state != lux::scene::EViewState::CLOSING &&
                                    view->result->value.status.state != lux::scene::EViewState::CLOSED;
            const bool ready =
                view_ready ||
                (scene && scene->status->state == ESceneResourceState::READY && scene->status->failure.ok()) ||
                (asset && asset->status.state == ERenderAssetState::READY) ||
                (output && output->info.texture.isValid() && output->produced.submitted() && !output->retired);
            if (!ready)
                return lux::cxx::unexpected(failure(render::ERendererError::NOT_READY));
        }
        {
            auto state = render::RenderSubmissionState::create(completion, &Impl::wake);
            if (!state)
                return lux::cxx::unexpected(failure(render::ERendererError::CAPACITY));
            for (auto* record : capture_records)
            {
                record->submissions.push_back(state->observe());
                enqueue(*record, false);
            }
            return std::move(*state);
        }
    }
    render::RenderResult<render::RenderSubmissionState> RenderResources::capture(std::span<const RenderResourceId> ids
    ) noexcept
    {
        impl_->requireOwner();
        if (impl_->closing)
            return lux::cxx::unexpected(failure(render::ERendererError::STOPPING));
        {
            auto& records = impl_->capture_records;
            records.clear();
            records.reserve(ids.size());
            for (const auto id : ids)
            {
                auto* record = impl_->find(id);
                if (!record)
                    return lux::cxx::unexpected(failure(render::ERendererError::INVALID_ARGUMENT));
                records.push_back(record);
            }
            return impl_->capture();
        }
    }
    render::RenderResult<render::RenderSubmissionState> RenderResources::captureTextures(
        std::span<const render::RTextureHandle> textures
    ) noexcept
    {
        impl_->requireOwner();
        if (impl_->closing)
            return lux::cxx::unexpected(failure(render::ERendererError::STOPPING));
        {
            auto& records = impl_->capture_records;
            records.clear();
            records.reserve(textures.size());
            for (const auto texture : textures)
            {
                const auto found = impl_->texture_records.find(texture);
                if (found == impl_->texture_records.end())
                    return lux::cxx::unexpected(failure(render::ERendererError::INVALID_ARGUMENT));
                records.push_back(found->second);
            }
            return impl_->capture();
        }
    }
    render::RenderResult<RenderAssetStatus> RenderResources::status(RenderResourceId id) const noexcept
    {
        const auto* record = impl_->find(id);
        if (!record || !std::holds_alternative<detail::AssetState>(record->payload))
            return lux::cxx::unexpected(failure(render::ERendererError::INVALID_ARGUMENT));
        return record->asset().status;
    }
    namespace
    {
        template <class State, class Record>
        auto handle(const Record* record) noexcept -> render::RenderResult<decltype(State{}.handle)>
        {
            const bool is_asset = record && std::holds_alternative<detail::AssetState>(record->payload);
            const auto* state = is_asset ? std::get_if<State>(&record->asset().payload) : nullptr;
            if (!state)
                return lux::cxx::unexpected(failure(render::ERendererError::INVALID_ARGUMENT));
            if (record->asset().status.state != ERenderAssetState::READY)
                return lux::cxx::unexpected(failure(render::ERendererError::NOT_READY));
            return state->handle;
        }
    }
    render::RenderResult<render::RMeshHandle> RenderResources::mesh(RenderResourceId id) const noexcept
    {
        return handle<detail::MeshState>(impl_->find(id));
    }
    render::RenderResult<render::RMaterialHandle> RenderResources::material(RenderResourceId id) const noexcept
    {
        return handle<detail::MaterialState>(impl_->find(id));
    }
    render::RenderResult<render::RTextureHandle> RenderResources::texture(RenderResourceId id) const noexcept
    {
        const auto* record = impl_->find(id);
        if (const auto* output = record ? std::get_if<detail::OutputState>(&record->payload) : nullptr)
        {
            if (!output->info.texture.isValid() || !output->produced.submitted() || output->retired)
                return lux::cxx::unexpected(failure(render::ERendererError::NOT_READY));
            return output->info.texture;
        }
        return handle<detail::TextureState>(record);
    }

    std::size_t RenderResources::viewCount() const noexcept
    {
        impl_->requireOwner();
        return std::ranges::count_if(impl_->records, [](const auto& record) {
            return std::holds_alternative<detail::EViewState>(record->payload);
        });
    }
    void RenderResources::Impl::adoptCompleted() noexcept
    {
        requireOwner();
        if (std::exchange(polling, true))
            render::renderFatal("Reentrant RenderResources poll");
        if (!backend_stopping && runtime.status().state != render::ERenderRuntimeState::ACTIVE)
        {
            backend_stopping = true;
            for (auto& entry : records)
                if (!std::holds_alternative<detail::AssetState>(entry->payload))
                    enqueue(*entry, false);
        }
        batch.swap(active);
        for (auto key : batch)
        {
            auto& record = *records[key];
            record.queued = false;
            std::erase_if(record.submissions, [](const auto& use) { return use.complete(); });
            if (auto* view = std::get_if<detail::EViewState>(&record.payload))
            {
                const auto before = view->result->value.status.state;
                advanceView(record);
                const auto phase = view->result->value.status.state;
                if (before != phase)
                    work.request();
                const bool retired = phase == lux::scene::EViewState::CLOSED;
                const bool pending = view->pending.isValid() || view->create.valid() || view->switch_target.valid() ||
                                     view->resize.valid() || phase == lux::scene::EViewState::CREATING ||
                                     phase == lux::scene::EViewState::RESIZING;
                if (!record.references && retired)
                    records.erase(key);
                else if (pending || !record.references || backend_stopping || !record.submissions.empty())
                    enqueue(record, false);
                continue;
            }
            if (auto* output = std::get_if<detail::OutputState>(&record.payload))
            {
                const auto before = output->info.texture;
                advanceOutput(record);
                if (before != output->info.texture)
                    work.request();
                if (!record.references && output->retired)
                {
                    --output->owner->outputs;
                    work.request();
                    records.erase(key);
                }
                else if (!record.references || output->create.valid() || !output->admitted ||
                         !record.submissions.empty())
                    enqueue(record, false);
                continue;
            }
            if (auto* scene = std::get_if<detail::SceneState>(&record.payload))
            {
                const auto before = scene->status->state;
                advanceScene(record);
                const auto phase = scene->status->state;
                if (before != phase)
                    work.request();
                const bool retired = phase == ESceneResourceState::RETIRED;
                const bool pending = phase != ESceneResourceState::READY && !retired;
                const bool stopping = backend_stopping && !retired;
                if (!record.references && retired)
                    records.erase(key);
                else if (pending || stopping || !record.references || !record.submissions.empty())
                    enqueue(record, false);
                continue;
            }
            if (!record.references)
            {
                if (retire(record))
                {
                    if (auto* material = std::get_if<detail::MaterialState>(&record.asset().payload))
                        for (auto dependency : material->textures)
                            if (dependency.isValid())
                                release(dependency);
                    records.erase(key);
                }
                else
                    enqueue(record, false);
                continue;
            }
            const auto before = record.asset().status.state;
            accept(record);
            if (!terminal(record.asset().status.state) && record.asset().status.state != ERenderAssetState::READY)
            {
                if (runtime.status().state == render::ERenderRuntimeState::ACTIVE)
                    prepare(record);
                if (!terminal(record.asset().status.state) && record.asset().status.state != ERenderAssetState::READY)
                    enqueue(record, false);
            }
            if (before != record.asset().status.state)
                work.request();
            if (!record.submissions.empty())
                enqueue(record, false);
        }
        batch.clear();
        polling = false;
    }
    void RenderResources::beginClose() noexcept
    {
        impl_->requireOwner();
        impl_->closing = true;
    }
    bool RenderResources::empty() const noexcept
    {
        impl_->requireOwner();
        return impl_->records.empty();
    }
    bool RenderResources::uses(const render::RenderRuntime& runtime) const noexcept
    {
        return &impl_->runtime == &runtime;
    }
} // namespace lux::scene
