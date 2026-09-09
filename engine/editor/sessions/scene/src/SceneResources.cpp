#include <lux/engine/editor/sessions/scene/detail/SceneResources.hpp>
#include <algorithm>
#include <limits>

namespace lux::editor::sessions::detail
{
    namespace
    {
        auto fail(ESceneError code, SessionId id) noexcept
        {
            return lux::cxx::unexpected(SceneFailure{code, id});
        }
        bool terminal(ESceneResourceState state) noexcept
        {
            return state == ESceneResourceState::FAILED || state == ESceneResourceState::CANCELLED ||
                   state == ESceneResourceState::SUPERSEDED || state == ESceneResourceState::RELEASED ||
                   state == ESceneResourceState::UNREFERENCED;
        }
        struct RetirementMarker final
        {
            std::shared_ptr<std::atomic<bool>> consumed;
            explicit RetirementMarker(std::shared_ptr<std::atomic<bool>> value) noexcept : consumed(std::move(value))
            {
            }
            ~RetirementMarker() noexcept
            {
                consumed->store(true, std::memory_order_release);
            }
        };
        template <class T> bool readDone(const AssetResult<T> &value) noexcept
        {
            return value.state.load(std::memory_order_acquire) != AssetResult<T>::EState::PENDING;
        }
        template <class T> void acceptAsset(const AssetResult<T> &value, SceneResourceRow &row) noexcept
        {
            if (terminal(row.state))
                return;
            const auto state = value.state.load(std::memory_order_acquire);
            if (state == AssetResult<T>::EState::ERROR)
            {
                row.asset_failure = value.failure;
                row.state = ESceneResourceState::FAILED;
            }
            else if (state == AssetResult<T>::EState::CANCELLED)
                row.state = ESceneResourceState::CANCELLED;
        }
    } // namespace
    void ResourceRequest::start(ResourceTasks &tasks, lux::process::asset_loading::AssetReadPort port) noexcept
    {
        if (row.key.mesh.isNull() || row.key.material.isNull())
        {
            row.state = ESceneResourceState::UNREFERENCED;
            mesh_read->state.store(AssetResult<lux::asset::MeshAsset>::EState::CANCELLED);
            material_read->state.store(AssetResult<lux::asset::MaterialAsset>::EState::CANCELLED);
            return;
        }
        auto first = tasks.read(port, row.key.mesh, mesh_read);
        if (!first)
        {
            row.process_failure = first.error();
            row.state = ESceneResourceState::FAILED;
            mesh_read->state.store(AssetResult<lux::asset::MeshAsset>::EState::CANCELLED);
        }
        auto second = tasks.read(std::move(port), row.key.material, material_read);
        if (!second)
        {
            row.process_failure = second.error();
            row.state = ESceneResourceState::FAILED;
            material_read->state.store(AssetResult<lux::asset::MaterialAsset>::EState::CANCELLED);
        }
    }
    void ResourceRequest::acceptReplies() noexcept
    {
        const auto accept = [this](auto &request, auto &handle, auto field)
        {
            if (!request.valid() || !request.isReady())
                return;
            const auto result = request.tryResult();
            if (result)
            {
                handle = result->get().*field;
                if (result->get().status && !terminal(row.state))
                {
                    row.backend_status = result->get().status;
                    row.state = ESceneResourceState::FAILED;
                }
            }
            else if (!terminal(row.state))
            {
                row.render_failure = result.error();
                row.state = ESceneResourceState::FAILED;
            }
            request = {};
        };
        accept(mesh_request, mesh, &lux::render::MeshUploadedReply::handle);
        accept(material_request, material, &lux::render::MaterialUploadedReply::handle);
        accept(forward_request, forward, &lux::render::ShaderCompiledReply::shader);
        accept(gbuffer_request, gbuffer, &lux::render::ShaderCompiledReply::shader);
        acceptAsset(*mesh_read, row);
        acceptAsset(*material_read, row);
    }
    bool ResourceRequest::settled() const noexcept
    {
        return readDone(*mesh_read) && readDone(*material_read) && !mesh_request.valid() && !material_request.valid() &&
               !forward_request.valid() && !gbuffer_request.valid();
    }
    void ResourceRequest::prepareStep(rendering::EditorRenderer &renderer, lux::scene::RenderRuntimeLease &runtime)
    {
        if (terminal(row.state) || row.state == ESceneResourceState::READY)
            return;
        if (!readDone(*mesh_read) || !readDone(*material_read))
            return;
        if (!mesh_read->value || !material_read->value)
            return;
        row.state = ESceneResourceState::UPLOADING;
        const auto mesh_ops = runtime.features().ops<lux::render::MeshStackOperationIds>("StandardMeshStack");
        const auto material_ops = runtime.features().ops<lux::render::MaterialOperationIds>("StandardMaterial");
        const auto failedUpload = [this](auto error)
        {
            if (error == lux::render::ERenderUploadSubmitError::QUEUE_FULL ||
                error == lux::render::ERenderUploadSubmitError::BYTE_BUDGET_EXHAUSTED)
                return;
            row.upload_failure = error;
            row.state = ESceneResourceState::FAILED;
        };
        if (!mesh.isValid() && !mesh_request.valid())
        {
            auto request = lux::render::uploadMesh(lux::render::MeshStackUploadClient{runtime.upload(), mesh_ops},
                                                   mesh_read->value->data());
            if (request)
                mesh_request = std::move(*request);
            else
                failedUpload(request.error());
        }
        if (terminal(row.state))
            return;
        const auto &material_data = material_read->value->data();
        if (!forward.isValid() && !forward_request.valid() && renderer.controlAvailable())
        {
            const auto info = lux::rdesc::ShaderInfo::serialize(material_data.forward_info);
            forward_request =
                runtime.control().compileShader(std::as_bytes(std::span{material_data.forward_spirv}), info);
        }
        if (!gbuffer.isValid() && !gbuffer_request.valid() && renderer.controlAvailable())
        {
            const auto info = lux::rdesc::ShaderInfo::serialize(material_data.gbuffer_info);
            gbuffer_request =
                runtime.control().compileShader(std::as_bytes(std::span{material_data.gbuffer_spirv}), info);
        }
        if (forward.isValid() && gbuffer.isValid() && !material.isValid() && !material_request.valid())
        {
            lux::render::GraphMaterialData data{};
            data.param_count = material_data.parameter_count;
            for (std::size_t i = 0; i < data.param_count; ++i)
                std::copy_n(material_data.parameter_defaults[i].data(), 4, data.params[i]);
            auto request = lux::render::uploadGraphMaterial(
                lux::render::MaterialUploadClient{runtime.upload(), material_ops}, data, gbuffer, forward,
                static_cast<std::uint32_t>(material_data.alpha_mode), material_data.double_sided);
            if (request)
                material_request = std::move(*request);
            else
                failedUpload(request.error());
        }
    }
    void ResourceRequest::releaseStep(rendering::EditorRenderer &renderer, lux::scene::RenderRuntimeLease &runtime)
    {
        if (!settled())
            return;
        if (renderer.state() == rendering::ERendererState::STOPPED ||
            renderer.state() == rendering::ERendererState::FAILED)
        {
            mesh = {};
            material = {};
            forward = {};
            gbuffer = {};
            return;
        }
        // An adopted handle can still occur in an accepted but unconsumed Scene upsert. The marker
        // is ordered after those updates and retires only when the actual SPSC consumer releases its slot.
        if (adopted && (!retired_program_consumed || !retired_program_consumed->load(std::memory_order_acquire)))
            return;
        if (!renderer.controlAvailable())
            return;
        if (mesh.isValid())
        {
            const auto ops = runtime.features().ops<lux::render::MeshStackOperationIds>("StandardMeshStack");
            lux::render::MeshStackControlClient{runtime.control(), ops}.destroyMesh({mesh});
            mesh = {};
            return;
        }
        if (material.isValid())
        {
            const auto ops = runtime.features().ops<lux::render::MaterialOperationIds>("StandardMaterial");
            lux::render::MaterialControlClient{runtime.control(), ops}.destroyMaterial({material});
            material = {};
            return;
        }
        if (forward.isValid())
        {
            runtime.control().destroyShader(forward);
            forward = {};
            return;
        }
        if (gbuffer.isValid())
        {
            runtime.control().destroyShader(gbuffer);
            gbuffer = {};
        }
    }
    SceneResources::SceneResources(SessionId id, lux::process::asset_loading::AssetReadPort port,
                                   rendering::EditorRenderer *renderer, std::size_t capacity)
        : session_(id), port_(std::move(port)), renderer_(renderer), capacity_(capacity)
    {
        requests_.reserve(capacity);
    }
    SceneResources::~SceneResources() noexcept
    {
        if (active_ && !closed_)
            std::terminate();
    }
    SceneResult<void> SceneResources::activate() noexcept
    {
        if (renderer_)
        {
            auto lease = renderer_->acquire();
            if (!lease)
                return fail(ESceneError::NOT_READY, session_);
            runtime_ = std::move(*lease);
        }
        active_ = true;
        return {};
    }
    SceneResult<bool> SceneResources::prepareUpdate(lux::simulation::ecs::Registry &registry) noexcept
    {
        if (closing_)
            return fail(ESceneError::CLOSED, session_);
        if (!renderer_)
            return false;
        bool changed{};
        try
        {
            // Retire obsolete identities before admitting replacements. Snapshots own their rows;
            // erasing an entirely released request cannot invalidate an observer's saved failure/key.
            for (auto &request : requests_)
            {
                const auto before = request->row.state;
                request->acceptReplies();
                changed |= request->row.state != before;
                const auto &key = request->row.key;
                const auto *visual = registry.valid(key.target.entity)
                                         ? registry.try_get<lux::simulation::ecs::Mesh3D>(key.target.entity)
                                         : nullptr;
                const bool current = visual && visual->value.mesh == key.mesh && visual->value.material == key.material;
                if (current)
                    continue;
                changed |= request->row.state != ESceneResourceState::SUPERSEDED;
                request->row.state = ESceneResourceState::SUPERSEDED;
                if (registry.valid(key.target.entity))
                {
                    const auto *resolved = registry.try_get<lux::scene::ResolvedMeshResources>(key.target.entity);
                    if (resolved && resolved->mesh == request->mesh && resolved->material == request->material)
                        registry.remove<lux::scene::ResolvedMeshResources>(key.target.entity);
                }
                request->releaseStep(*renderer_, runtime_);
            }
            const auto removed = std::erase_if(requests_,
                                               [](const auto &request)
                                               {
                                                   return request->row.state == ESceneResourceState::SUPERSEDED &&
                                                          request->settled() && !request->mesh.isValid() &&
                                                          !request->material.isValid() && !request->forward.isValid() &&
                                                          !request->gbuffer.isValid();
                                               });
            changed |= removed != 0;
            for (const auto entity : registry.view<lux::simulation::ecs::Mesh3D>())
            {
                const auto &visual = registry.get<lux::simulation::ecs::Mesh3D>(entity).value;
                const auto found = std::find_if(requests_.begin(), requests_.end(),
                                                [&](const auto &request)
                                                {
                                                    const auto &key = request->row.key;
                                                    return key.target.entity == entity && key.mesh == visual.mesh &&
                                                           key.material == visual.material &&
                                                           request->row.state != ESceneResourceState::SUPERSEDED;
                                                });
                if (found != requests_.end())
                    continue;
                if (requests_.size() == capacity_)
                {
                    const bool reclaiming =
                        std::any_of(requests_.begin(), requests_.end(), [](const auto &request)
                                    { return request->row.state == ESceneResourceState::SUPERSEDED; });
                    // Let presentation/retirement progress while obsolete slots are still owned.
                    if (reclaiming)
                        continue;
                    return fail(ESceneError::RESOURCE_FAILURE, session_);
                }
                if (sequence_ == (std::numeric_limits<std::uint64_t>::max)())
                    return fail(ESceneError::RESOURCE_FAILURE, session_);
                auto request = std::make_unique<ResourceRequest>(
                    ResourceRequestKey{{session_, entity}, visual.mesh, visual.material, sequence_ + 1});
                requests_.push_back(std::move(request));
                ++sequence_;
                requests_.back()->start(tasks_, port_);
                changed = true;
            }
            for (auto &request : requests_)
            {
                const auto before = request->row.state;
                request->acceptReplies();
                const auto &key = request->row.key;
                const auto *visual = registry.valid(key.target.entity)
                                         ? registry.try_get<lux::simulation::ecs::Mesh3D>(key.target.entity)
                                         : nullptr;
                const bool current = visual && visual->value.mesh == key.mesh && visual->value.material == key.material;
                if (!current)
                    request->row.state = ESceneResourceState::SUPERSEDED;
                if (terminal(request->row.state))
                    request->releaseStep(*renderer_, runtime_);
                else
                {
                    request->prepareStep(*renderer_, runtime_);
                    if (request->mesh.isValid() && request->material.isValid() &&
                        request->row.state != ESceneResourceState::READY && !terminal(request->row.state))
                    {
                        registry.emplace_or_replace<lux::scene::ResolvedMeshResources>(
                            key.target.entity, lux::scene::ResolvedMeshResources{key.mesh, key.material, request->mesh,
                                                                                 request->material});
                        request->row.state = ESceneResourceState::READY;
                        request->adopted = true;
                    }
                }
                changed |= request->row.state != before;
            }
            if (auto prepared = prepareRetirement(false); !prepared)
                return lux::cxx::unexpected(prepared.error());
            return changed;
        }
        catch (const std::bad_alloc &)
        {
            return fail(ESceneError::ALLOCATION_FAILURE, session_);
        }
    }
    SceneResult<void> SceneResources::prepareRetirement(bool all) noexcept
    {
        if (!renderer_ || retirement_pending_)
            return {};
        const auto needs_marker = [&](const auto &request)
        { return request->adopted && !request->retired_program_consumed && (all || terminal(request->row.state)); };
        if (std::none_of(requests_.begin(), requests_.end(), needs_marker))
            return {};
        try
        {
            auto consumed = std::make_shared<std::atomic<bool>>(false);
            lux::render::RenderProgram<> prepared;
            prepared.attachments.reserve(1);
            lux::render::RenderProgramSession::Builder builder(prepared);
            static_cast<void>(
                builder.emplaceAttachment<RetirementMarker>(lux::render::attachment_types::OwnedObject, consumed));
            retirement_program_ = std::move(prepared);
            retirement_pending_ = true;
            for (auto &request : requests_)
                if (needs_marker(request))
                    request->retired_program_consumed = consumed;
            return {};
        }
        catch (const std::bad_alloc &)
        {
            return fail(ESceneError::ALLOCATION_FAILURE, session_);
        }
    }
    void SceneResources::afterPresentation(bool source_update_pending) noexcept
    {
        if (!renderer_ || source_update_pending || renderer_->state() != rendering::ERendererState::READY)
            return;
        auto &programs = runtime_.programs();
        if (programs.hasPendingSubmit())
            return;
        if (retirement_pending_)
        {
            if (!programs.trySubmitPrepared(retirement_program_))
                return;
            retirement_pending_ = false;
        }
        const bool awaiting_consumption =
            std::any_of(requests_.begin(), requests_.end(),
                        [](const auto &request)
                        {
                            return request->retired_program_consumed &&
                                   !request->retired_program_consumed->load(std::memory_order_acquire);
                        });
        if (awaiting_consumption)
        {
            // One empty StateUpdate advances slot retirement without replacing the last UI draw or
            // executing Scene hooks. It is retried under ordinary channel backpressure, never waited on.
            static_cast<void>(programs.trySubmitPrepared(retirement_progress_));
        }
    }
    SceneResult<void> SceneResources::retry(const ResourceRequestKey &key) noexcept
    {
        if (closing_)
            return fail(ESceneError::CLOSED, session_);
        if (key.target.session != session_)
            return fail(ESceneError::STALE_SESSION, session_);
        const auto found =
            std::find_if(requests_.begin(), requests_.end(), [&](const auto &item) { return item->row.key == key; });
        if (found == requests_.end())
            return fail(ESceneError::STALE_CONTENT, session_);
        const auto &old = **found;
        const bool obsolete = old.row.state == ESceneResourceState::SUPERSEDED ||
                              old.row.state == ESceneResourceState::RELEASED ||
                              old.row.state == ESceneResourceState::UNREFERENCED;
        if (obsolete)
            return fail(ESceneError::STALE_CONTENT, session_);
        if (!terminal(old.row.state) || !old.settled() || old.mesh.isValid() || old.material.isValid() ||
            old.forward.isValid() || old.gbuffer.isValid())
            return fail(ESceneError::BUSY, session_);
        if (sequence_ == (std::numeric_limits<std::uint64_t>::max)())
            return fail(ESceneError::RESOURCE_FAILURE, session_);
        try
        {
            auto next = key;
            next.sequence = sequence_ + 1;
            auto replacement = std::make_unique<ResourceRequest>(next);
            *found = std::move(replacement);
            ++sequence_;
            (*found)->start(tasks_, port_);
            return {};
        }
        catch (const std::bad_alloc &)
        {
            return fail(ESceneError::ALLOCATION_FAILURE, session_);
        }
    }
    SceneResult<std::shared_ptr<const SceneResourceSnapshot>> SceneResources::snapshot(
        std::uint64_t revision) const noexcept
    {
        try
        {
            auto result = std::make_shared<SceneResourceSnapshot>();
            result->session = session_;
            result->revision = revision;
            result->rows.reserve(requests_.size());
            for (const auto &request : requests_)
                result->rows.push_back(request->row);
            return result;
        }
        catch (const std::bad_alloc &)
        {
            return fail(ESceneError::ALLOCATION_FAILURE, session_);
        }
    }
    SceneResult<void> SceneResources::beginClose() noexcept
    {
        closing_ = true;
        return tasks_.close();
    }
    SceneResult<bool> SceneResources::advanceClose() noexcept
    {
        if (!closing_)
            return fail(ESceneError::BUSY, session_);
        if (closed_)
            return true;
        // A failed close-operation allocation retains this owner. Advance retries that preparation itself.
        if (auto closed = tasks_.close(); !closed)
            return lux::cxx::unexpected(closed.error());
        if (auto prepared = prepareRetirement(true); !prepared)
            return lux::cxx::unexpected(prepared.error());
        afterPresentation(false);
        try
        {
            for (auto &request : requests_)
            {
                request->acceptReplies();
                request->releaseStep(*renderer_, runtime_);
                if (!request->settled() || request->mesh.isValid() || request->material.isValid() ||
                    request->forward.isValid() || request->gbuffer.isValid())
                    return false;
            }
            if (!tasks_.done.load(std::memory_order_acquire))
                return false;
            requests_.clear();
            runtime_ = {};
            closed_ = true;
            return true;
        }
        catch (const std::bad_alloc &)
        {
            return fail(ESceneError::ALLOCATION_FAILURE, session_);
        }
    }
} // namespace lux::editor::sessions::detail
