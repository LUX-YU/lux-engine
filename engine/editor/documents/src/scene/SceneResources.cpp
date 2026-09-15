#include <lux/engine/editor/scene/detail/SceneResources.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <algorithm>
#include <limits>

namespace lux::editor::scene::detail
{
    namespace
    {
        auto fail(ESceneError code, editing::HistoryId id) noexcept
        {
            return lux::cxx::unexpected(SceneFailure{code, id});
        }

        bool terminal(ESceneResourceState state) noexcept
        {
            return state == ESceneResourceState::FAILED || state == ESceneResourceState::CANCELLED ||
                   state == ESceneResourceState::SUPERSEDED || state == ESceneResourceState::RELEASED ||
                   state == ESceneResourceState::UNREFERENCED;
        }

        struct RowChange final
        {
            const SceneResourceRow &row;
            bool &pending;
            ESceneResourceState before;

            RowChange(const SceneResourceRow &value, bool &dirty) noexcept
                : row(value), pending(dirty), before(value.state)
            {
            }

            ~RowChange() noexcept
            {
                // State can change before a later preparation step returns an error.
                pending |= row.state != before;
            }
        };

        struct RetirementMarker final
        {
            std::shared_ptr<std::atomic<bool>> consumed;

            explicit RetirementMarker(std::shared_ptr<std::atomic<bool>> value) noexcept : consumed(std::move(value)) {}

            ~RetirementMarker() noexcept
            {
                consumed->store(true, std::memory_order_release);
            }
        };

        template <class T> bool readDone(const AssetResult<T> &value) noexcept
        {
            return value.state.load(std::memory_order_acquire) != AssetResult<T>::EState::PENDING;
        }

        template <class T>
        void acceptAsset(const AssetResult<T> &value, SceneResourceRow &row, lux::asset::AssetId asset) noexcept
        {
            if (terminal(row.state))
            {
                return;
            }
            const auto state = value.state.load(std::memory_order_acquire);
            if (state == AssetResult<T>::EState::ERROR)
            {
                row.failure = value.failure;
                row.failed_dependency = asset;
                row.state = ESceneResourceState::FAILED;
            }
            else if (state == AssetResult<T>::EState::CANCELLED)
            {
                row.state = ESceneResourceState::CANCELLED;
            }
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
            row.failure = first.error();
            row.state = ESceneResourceState::FAILED;
            mesh_read->state.store(AssetResult<lux::asset::MeshAsset>::EState::CANCELLED);
        }
        auto second = tasks.read(std::move(port), row.key.material, material_read);
        if (!second)
        {
            row.failure = second.error();
            row.state = ESceneResourceState::FAILED;
            material_read->state.store(AssetResult<lux::asset::MaterialAsset>::EState::CANCELLED);
        }
    }

    void ResourceRequest::acceptReplies() noexcept
    {
        const auto accept = [this](auto &request, auto &handle, auto field)
        {
            if (!request.valid() || !request.isReady())
            {
                return;
            }
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
        acceptAsset(*mesh_read, row, row.key.mesh);
        acceptAsset(*material_read, row, row.key.material);
        for (auto &texture : textures)
        {
            const bool was_terminal = terminal(row.state);
            accept(texture.upload, texture.handle, &lux::render::Texture2DCreatedReply::handle);
            acceptAsset(*texture.read, row, texture.asset);
            if (!was_terminal && terminal(row.state))
            {
                row.failed_dependency = texture.asset;
            }
        }
    }

    bool ResourceRequest::settled() const noexcept
    {
        return readDone(*mesh_read) && readDone(*material_read) && !mesh_request.valid() && !material_request.valid() &&
               !forward_request.valid() && !gbuffer_request.valid() &&
               std::all_of(textures.begin(), textures.end(),
                           [](const auto &texture) { return readDone(*texture.read) && !texture.upload.valid(); });
    }

    std::size_t ResourceRequest::liveHandles() const noexcept
    {
        return std::size_t(mesh.isValid()) + material.isValid() + forward.isValid() + gbuffer.isValid() +
               std::count_if(textures.begin(), textures.end(),
                             [](const auto &texture) { return texture.handle.isValid(); });
    }

    void ResourceRequest::prepareStep(rendering::EditorRenderer &renderer, lux::scene::RenderRuntimeLease &runtime,
                                      ResourceTasks &tasks, lux::process::asset_loading::AssetReadPort port)
    {
        if (terminal(row.state) || row.state == ESceneResourceState::READY)
        {
            return;
        }
        if (!readDone(*mesh_read) || !readDone(*material_read))
        {
            return;
        }
        if (!mesh_read->value || !material_read->value)
        {
            return;
        }
        row.state = ESceneResourceState::UPLOADING;
        const auto mesh_ops = runtime.features().ops<lux::render::MeshStackOperationIds>("StandardMeshStack");
        const auto material_ops = runtime.features().ops<lux::render::MaterialOperationIds>("StandardMaterial");
        const auto failedUpload = [this](auto error)
        {
            if (error == lux::render::ERenderUploadSubmitError::QUEUE_FULL ||
                error == lux::render::ERenderUploadSubmitError::BYTE_BUDGET_EXHAUSTED)
            {
                return;
            }
            row.failure = error;
            row.state = ESceneResourceState::FAILED;
        };
        if (!mesh.isValid() && !mesh_request.valid())
        {
            auto request = lux::render::uploadMesh(lux::render::MeshStackUploadClient{runtime.upload(), mesh_ops},
                                                   mesh_read->value->data());
            if (request)
            {
                mesh_request = std::move(*request);
            }
            else
            {
                failedUpload(request.error());
            }
        }
        if (terminal(row.state))
        {
            return;
        }
        const auto &material_data = material_read->value->data();
        if (!texture_reads_started)
        {
            for (const auto id : material_data.texture_slot_ids)
            {
                if (id.isNull())
                {
                    continue;
                }
                const bool already_read =
                    std::any_of(textures.begin(), textures.end(), [&](const auto &item) { return item.asset == id; });
                if (already_read)
                {
                    continue;
                }
                textures.push_back({id, std::make_shared<AssetResult<lux::asset::TextureAsset>>()});
            }
            texture_reads_started = true;
            for (auto &texture : textures)
            {
                const auto started = tasks.read(port, texture.asset, texture.read);
                if (!started)
                {
                    if (!terminal(row.state))
                    {
                        row.failure = started.error();
                        row.failed_dependency = texture.asset;
                        row.state = ESceneResourceState::FAILED;
                    }
                    texture.read->state.store(AssetResult<lux::asset::TextureAsset>::EState::CANCELLED);
                }
            }
        }
        if (terminal(row.state))
        {
            return;
        }
        for (auto &texture : textures)
        {
            if (texture.handle.isValid() || texture.upload.valid() || !readDone(*texture.read))
            {
                continue;
            }
            if (!texture.read->value)
            {
                continue; // acceptReplies preserves the subordinate error/cancellation.
            }
            const auto &image = texture.read->value->data();
            if (image.layers() != 1)
            {
                row.failure = lux::render::ERenderUploadSubmitError::PAYLOAD_INVALID;
                row.failed_dependency = texture.asset;
                row.state = ESceneResourceState::FAILED;
                return;
            }
            std::vector<lux::render::OwnedTextureMipLevel> levels;
            levels.reserve(image.mipCount());
            for (std::uint32_t mip{}; mip < image.mipCount(); ++mip)
            {
                const auto &range = image.mipRange(mip);
                levels.push_back({image.pixels().subspan(static_cast<std::size_t>(range.offset),
                                                         static_cast<std::size_t>(range.size)),
                                  range.width, range.height});
            }
            const bool generate_mips =
                image.mipCount() == 1 && !lux::rdesc::isCompressedFormat(image.pixelFormat()) &&
                !lux::rdesc::hasTextureFlag(image.flags(), lux::rdesc::ETextureAssetFlags::NO_MIPS);
            auto uploaded = runtime.upload().tryCreateTexture2DMips(std::move(levels), image.channel(),
                                                                    image.pixelFormat(), generate_mips);
            if (uploaded)
            {
                texture.upload = std::move(*uploaded);
            }
            else
            {
                failedUpload(uploaded.error());
                if (terminal(row.state))
                {
                    row.failed_dependency = texture.asset;
                    return;
                }
            }
        }
        if (!forward.isValid() && !forward_request.valid() && renderer.controlAvailable())
        {
            auto info = lux::rdesc::ShaderInfo::serialize(material_data.forward_info);
            forward_request =
                runtime.control().compileShader(std::as_bytes(std::span{material_data.forward_spirv}), info);
        }
        if (!gbuffer.isValid() && !gbuffer_request.valid() && renderer.controlAvailable())
        {
            const auto info = lux::rdesc::ShaderInfo::serialize(material_data.gbuffer_info);
            gbuffer_request =
                runtime.control().compileShader(std::as_bytes(std::span{material_data.gbuffer_spirv}), info);
        }
        const bool textures_ready =
            std::all_of(textures.begin(), textures.end(), [](const auto &texture) { return texture.handle.isValid(); });
        const bool can_upload_material = forward.isValid() && gbuffer.isValid() && textures_ready &&
                                         !material.isValid() && !material_request.valid();
        if (can_upload_material)
        {
            lux::render::GraphMaterialData data{};
            data.param_count = material_data.parameter_count;
            for (std::size_t i = 0; i < data.param_count; ++i)
            {
                std::copy_n(material_data.parameter_defaults[i].data(), 4, data.params[i]);
            }
            for (std::size_t slot{}; slot < material_data.texture_slot_ids.size(); ++slot)
            {
                const auto id = material_data.texture_slot_ids[slot];
                if (id.isNull())
                {
                    continue;
                }
                const auto texture =
                    std::find_if(textures.begin(), textures.end(), [&](const auto &item) { return item.asset == id; });
                data.tex_mask |= 1U << slot;
                data.tex_bindless[slot] = texture->handle.index;
            }
            auto request = lux::render::uploadGraphMaterial(
                lux::render::MaterialUploadClient{runtime.upload(), material_ops}, data, gbuffer, forward,
                static_cast<std::uint32_t>(material_data.alpha_mode), material_data.double_sided);
            if (request)
            {
                material_request = std::move(*request);
            }
            else
            {
                failedUpload(request.error());
            }
        }
    }

    void ResourceRequest::releaseStep(rendering::EditorRenderer &renderer, lux::scene::RenderRuntimeLease &runtime)
    {
        if (!settled())
        {
            return;
        }
        if (renderer.state() == rendering::ERendererState::STOPPED ||
            renderer.state() == rendering::ERendererState::FAILED)
        {
            mesh = {};
            material = {};
            forward = {};
            gbuffer = {};
            for (auto &texture : textures)
            {
                texture.handle = {};
            }
            return;
        }
        // An adopted handle can still occur in an accepted but unconsumed Scene upsert. The marker
        // is ordered after those updates and retires only when the actual SPSC consumer releases its slot.
        if (adopted && (!retired_program_consumed || !retired_program_consumed->load(std::memory_order_acquire)))
        {
            return;
        }
        if (!renderer.controlAvailable())
        {
            return;
        }
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
            return;
        }
        for (auto &texture : textures)
        {
            if (!texture.handle.isValid())
            {
                continue;
            }
            runtime.control().destroyTexture(texture.handle);
            texture.handle = {};
            return;
        }
    }

    SceneResources::SceneResources(editing::HistoryId id, lux::process::asset_loading::AssetReadPort port,
                                   rendering::EditorRenderer *renderer, std::size_t capacity)
        : history_(id), port_(std::move(port)), renderer_(renderer), capacity_(capacity)
    {
        requests_.reserve(capacity);
        current_requests_.reserve(capacity);
    }

    SceneResources::~SceneResources() noexcept
    {
        if (active_ && !closed_)
        {
            std::terminate();
        }
    }

    SceneResult<void> SceneResources::activate() noexcept
    {
        if (renderer_)
        {
            auto lease = renderer_->acquire();
            if (!lease)
            {
                return fail(ESceneError::NOT_READY, history_);
            }
            runtime_ = std::move(*lease);
        }
        active_ = true;
        return {};
    }

    SceneResult<bool> SceneResources::prepareUpdate(lux::simulation::ecs::Registry &registry) noexcept
    {
        if (closing_)
        {
            return fail(ESceneError::CLOSED, history_);
        }
        if (!renderer_)
        {
            return false;
        }
        auto &changed = pending_change_;
        {
            // Retire obsolete identities before admitting replacements. Snapshots own their rows;
            // erasing an entirely released request cannot invalidate an observer's saved failure/key.
            for (auto &request : requests_)
            {
                const RowChange change{request->row, changed};
                request->acceptReplies();
                const auto &key = request->row.key;
                const auto *visual = registry.valid(key.target.entity)
                                         ? registry.try_get<lux::simulation::ecs::Mesh3D>(key.target.entity)
                                         : nullptr;
                const bool current = visual && visual->value.mesh == key.mesh && visual->value.material == key.material;
                if (current)
                {
                    continue;
                }
                if (request->refresh_sequence)
                {
                    --refresh_reservations_;
                    request->refresh_sequence = 0;
                    request->row.refresh_pending = false;
                }
                const auto association = current_requests_.find(key.target.entity);
                if (association != current_requests_.end() && association->second == request.get())
                {
                    current_requests_.erase(association);
                }
                changed |= request->row.state != ESceneResourceState::SUPERSEDED;
                request->row.state = ESceneResourceState::SUPERSEDED;
                if (registry.valid(key.target.entity))
                {
                    const auto *resolved = registry.try_get<lux::scene::ResolvedMeshResources>(key.target.entity);
                    if (resolved && resolved->mesh == request->mesh && resolved->material == request->material)
                    {
                        registry.remove<lux::scene::ResolvedMeshResources>(key.target.entity);
                    }
                }
                request->releaseStep(*renderer_, runtime_);
            }
            const auto removed = std::erase_if(requests_,
                                               [](const auto &request)
                                               {
                                                   return request->row.state == ESceneResourceState::SUPERSEDED &&
                                                          request->settled() && request->liveHandles() == 0;
                                               });
            changed |= removed != 0;
            for (const auto entity : registry.view<lux::simulation::ecs::Mesh3D>())
            {
                const auto &visual = registry.get<lux::simulation::ecs::Mesh3D>(entity).value;
                const auto found = current_requests_.find(entity);
                if (found != current_requests_.end())
                {
                    const auto &key = found->second->row.key;
                    const bool matches = key.mesh == visual.mesh && key.material == visual.material &&
                                         found->second->row.state != ESceneResourceState::SUPERSEDED;
                    if (!matches)
                    {
                        return fail(ESceneError::STALE_CONTENT, history_);
                    }
                    auto &previous = *found->second;
                    if (previous.refresh_sequence)
                    {
                        if (requests_.size() == capacity_)
                        {
                            continue;
                        }
                        auto next = previous.row.key;
                        next.sequence = previous.refresh_sequence;
                        auto replacement = std::make_unique<ResourceRequest>(next);
                        found->second = replacement.get();
                        requests_.push_back(std::move(replacement));
                        --refresh_reservations_;
                        previous.refresh_sequence = 0;
                        previous.row.refresh_pending = false;
                        // Keep the last adopted row/Registry handles until its successor can atomically replace them.
                        if (!previous.adopted)
                        {
                            previous.row.state = ESceneResourceState::SUPERSEDED;
                        }
                        requests_.back()->start(tasks_, port_);
                        changed = true;
                    }
                    continue;
                }
                if (requests_.size() + refresh_reservations_ == capacity_)
                {
                    if (refresh_reservations_)
                    {
                        continue;
                    }
                    const bool reclaiming =
                        std::any_of(requests_.begin(), requests_.end(), [](const auto &request)
                                    { return request->row.state == ESceneResourceState::SUPERSEDED; });
                    // Let presentation/retirement progress while obsolete slots are still owned.
                    if (reclaiming)
                    {
                        continue;
                    }
                    return fail(ESceneError::RESOURCE_FAILURE, history_);
                }
                if (sequence_ == (std::numeric_limits<std::uint64_t>::max)())
                {
                    return fail(ESceneError::RESOURCE_FAILURE, history_);
                }
                auto request = std::make_unique<ResourceRequest>(
                    ResourceRequestKey{{history_, entity}, visual.mesh, visual.material, sequence_ + 1});
                // Finish all allocating preparation before admission/start. The entries vector was reserved
                // at construction and remains below capacity; moving unique_ptr into it cannot fail.
                current_requests_.emplace(entity, request.get());
                requests_.push_back(std::move(request));
                ++sequence_;
                requests_.back()->start(tasks_, port_);
                changed = true;
            }
            for (auto &request : requests_)
            {
                const RowChange change{request->row, changed};
                request->acceptReplies();
                const auto &key = request->row.key;
                const auto *visual = registry.valid(key.target.entity)
                                         ? registry.try_get<lux::simulation::ecs::Mesh3D>(key.target.entity)
                                         : nullptr;
                const bool current = visual && visual->value.mesh == key.mesh && visual->value.material == key.material;
                if (!current)
                {
                    request->row.state = ESceneResourceState::SUPERSEDED;
                }
                if (terminal(request->row.state))
                {
                    request->releaseStep(*renderer_, runtime_);
                }
                else
                {
                    request->prepareStep(*renderer_, runtime_, tasks_, port_);
                    if (request->mesh.isValid() && request->material.isValid() &&
                        request->row.state != ESceneResourceState::READY && !terminal(request->row.state))
                    {
                        registry.emplace_or_replace<lux::scene::ResolvedMeshResources>(
                            key.target.entity, lux::scene::ResolvedMeshResources{key.mesh, key.material, request->mesh,
                                                                                 request->material});
                        request->row.state = ESceneResourceState::READY;
                        request->adopted = true;
                        for (auto &previous : requests_)
                        {
                            if (previous.get() == request.get() || previous->row.key.target != key.target)
                            {
                                continue;
                            }
                            if (previous->adopted)
                            {
                                previous->row.state = ESceneResourceState::SUPERSEDED;
                            }
                        }
                    }
                }
            }
            if (auto prepared = prepareRetirement(false); !prepared)
            {
                return lux::cxx::unexpected(prepared.error());
            }
            return changed;
        }
    }

    bool SceneResources::hasPendingWork() const noexcept
    {
        return pending_change_ || retirement_pending_ || refresh_reservations_ ||
               std::ranges::any_of(requests_,
                                   [](const auto &request)
                                   {
                                       return request->row.state == ESceneResourceState::READING ||
                                              request->row.state == ESceneResourceState::UPLOADING ||
                                              request->row.state == ESceneResourceState::SUPERSEDED ||
                                              !request->settled() || request->refresh_sequence ||
                                              (request->row.state != ESceneResourceState::READY &&
                                               request->liveHandles() != 0);
                                   });
    }

    void SceneResources::acknowledgeSnapshot() noexcept
    {
        // The document acknowledges only after publishing the complete snapshot.
        pending_change_ = false;
    }

    SceneResult<void> SceneResources::prepareRetirement(bool all) noexcept
    {
        if (!renderer_ || retirement_pending_)
        {
            return {};
        }
        const auto needs_marker = [&](const auto &request)
        { return request->adopted && !request->retired_program_consumed && (all || terminal(request->row.state)); };
        if (std::none_of(requests_.begin(), requests_.end(), needs_marker))
        {
            return {};
        }
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
            {
                if (needs_marker(request))
                {
                    request->retired_program_consumed = consumed;
                }
            }
            return {};
        }
    }

    void SceneResources::afterPresentation(bool source_update_pending) noexcept
    {
        // A document can close before its first poll activates the resource owner.
        // In that state there is no runtime lease or submitted program to advance.
        if (!active_ || closed_ || !renderer_ || source_update_pending ||
            renderer_->state() != rendering::ERendererState::READY)
        {
            return;
        }
        auto &programs = runtime_.programs();
        if (programs.hasPendingSubmit())
        {
            return;
        }
        if (retirement_pending_)
        {
            if (!programs.trySubmitPrepared(retirement_program_))
            {
                return;
            }
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

    SceneResult<std::size_t> SceneResources::refreshAssets(std::span<const lux::asset::AssetId> assets) noexcept
    {
        if (closing_)
        {
            return fail(ESceneError::CLOSED, history_);
        }
        const bool has_null = std::any_of(assets.begin(), assets.end(), [](const auto id) { return id.isNull(); });
        if (has_null)
        {
            return fail(ESceneError::INVALID_ARGUMENT, history_);
        }
        const auto matches = [&](const ResourceRequest &request)
        {
            const auto contains = [&](lux::asset::AssetId id)
            { return std::find(assets.begin(), assets.end(), id) != assets.end(); };
            return contains(request.row.key.mesh) || contains(request.row.key.material) ||
                   std::any_of(request.textures.begin(), request.textures.end(),
                               [&](const auto &texture) { return contains(texture.asset); });
        };
        const auto count =
            static_cast<std::size_t>(std::count_if(current_requests_.begin(), current_requests_.end(),
                                                   [&](const auto &entry) { return matches(*entry.second); }));
        const auto additional = static_cast<std::size_t>(
            std::count_if(current_requests_.begin(), current_requests_.end(), [&](const auto &entry)
                          { return !entry.second->refresh_sequence && matches(*entry.second); }));
        const bool capacity_exhausted = additional > capacity_ - requests_.size() - refresh_reservations_;
        const bool identity_exhausted = count > (std::numeric_limits<std::uint64_t>::max)() - sequence_;
        if (capacity_exhausted || identity_exhausted)
        {
            return fail(ESceneError::RESOURCE_FAILURE, history_);
        }
        refresh_reservations_ += additional;
        for (const auto &[entity, request] : current_requests_)
        {
            if (!matches(*request))
            {
                continue;
            }
            request->refresh_sequence = ++sequence_;
            request->row.refresh_pending = true;
        }
        pending_change_ |= count != 0;
        return count;
    }

    SceneResult<void> SceneResources::retry(const ResourceRequestKey &key) noexcept
    {
        if (closing_)
        {
            return fail(ESceneError::CLOSED, history_);
        }
        if (key.target.history != history_)
        {
            return fail(ESceneError::STALE_DOCUMENT, history_);
        }
        const auto found =
            std::find_if(requests_.begin(), requests_.end(), [&](const auto &item) { return item->row.key == key; });
        if (found == requests_.end())
        {
            return fail(ESceneError::STALE_CONTENT, history_);
        }
        const auto &old = **found;
        const bool obsolete = old.row.state == ESceneResourceState::SUPERSEDED ||
                              old.row.state == ESceneResourceState::RELEASED ||
                              old.row.state == ESceneResourceState::UNREFERENCED;
        if (obsolete)
        {
            return fail(ESceneError::STALE_CONTENT, history_);
        }
        if (old.refresh_sequence || !terminal(old.row.state) || !old.settled() || old.liveHandles())
        {
            return fail(ESceneError::BUSY, history_);
        }
        if (sequence_ == (std::numeric_limits<std::uint64_t>::max)())
        {
            return fail(ESceneError::RESOURCE_FAILURE, history_);
        }
        {
            const auto association = current_requests_.find(key.target.entity);
            if (association == current_requests_.end() || association->second != found->get())
            {
                return fail(ESceneError::STALE_CONTENT, history_);
            }
            auto next = key;
            next.sequence = sequence_ + 1;
            auto replacement = std::make_unique<ResourceRequest>(next);
            *found = std::move(replacement);
            association->second = found->get();
            ++sequence_;
            (*found)->start(tasks_, port_);
            pending_change_ = true;
            return {};
        }
    }

    SceneResult<std::shared_ptr<const SceneResourceSnapshot>> SceneResources::snapshot(
        std::uint64_t revision) const noexcept
    {
        {
            auto result = std::make_shared<SceneResourceSnapshot>();
            result->history = history_;
            result->revision = revision;
            result->rows.reserve(requests_.size());
            for (const auto &request : requests_)
            {
                result->rows.push_back(request->row);
            }
            return result;
        }
    }

    SceneResult<std::shared_ptr<const SceneCloseSnapshot>> SceneResources::closeSnapshot(
        ECloseState state, std::size_t views, bool scene_present) const noexcept
    {
        {
            auto result = std::make_shared<SceneCloseSnapshot>();
            result->history = history_;
            result->state = state;
            result->views = views;
            result->scene_present = scene_present;
            result->task_scope_complete = tasks_.done.load(std::memory_order_acquire);
            result->retirement_submission_pending = retirement_pending_;
            result->resources.reserve(requests_.size());
            for (const auto &request : requests_)
            {
                SceneResourceCloseRow row;
                row.resource = request->row;
                row.mesh_read_pending = !readDone(*request->mesh_read);
                row.material_read_pending = !readDone(*request->material_read);
                row.mesh_upload_pending = request->mesh_request.valid();
                row.material_upload_pending = request->material_request.valid();
                row.forward_upload_pending = request->forward_request.valid();
                row.gbuffer_upload_pending = request->gbuffer_request.valid();
                row.retirement_pending = request->retired_program_consumed &&
                                         !request->retired_program_consumed->load(std::memory_order_acquire);
                row.live_handles = request->liveHandles();
                for (const auto &texture : request->textures)
                {
                    row.texture_reads_pending += !readDone(*texture.read);
                    row.texture_uploads_pending += texture.upload.valid();
                }
                result->resources.push_back(std::move(row));
            }
            return result;
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
        {
            return fail(ESceneError::BUSY, history_);
        }
        if (closed_)
        {
            return true;
        }
        // A failed close-operation allocation retains this owner. Advance retries that preparation itself.
        if (auto closed = tasks_.close(); !closed)
        {
            return lux::cxx::unexpected(closed.error());
        }
        if (auto prepared = prepareRetirement(true); !prepared)
        {
            return lux::cxx::unexpected(prepared.error());
        }
        afterPresentation(false);
        {
            for (auto &request : requests_)
            {
                request->acceptReplies();
                request->releaseStep(*renderer_, runtime_);
                if (!request->settled() || request->liveHandles())
                {
                    return false;
                }
            }
            if (!tasks_.done.load(std::memory_order_acquire))
            {
                return false;
            }
            current_requests_.clear();
            requests_.clear();
            runtime_ = {};
            closed_ = true;
            return true;
        }
    }
} // namespace lux::editor::scene::detail
