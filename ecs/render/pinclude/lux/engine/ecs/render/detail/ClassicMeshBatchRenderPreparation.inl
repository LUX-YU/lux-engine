        [[nodiscard]] bool fillTopology(
            lux::ecs::Entity entity,
            lux::render::UploadRenderClusterPayload& payload) const noexcept
        {
            if (!registry)
                return false;
            if (const auto* lod = registry->try_get<
                    lux::ecs::VisualLodNodeComponent>(entity))
            {
                if (!detail::validVisualLodContract(
                        lod->geometric_error,
                        lod->enter_error_pixels,
                        lod->exit_error_pixels))
                {
                    return false;
                }
                payload.lod_level = lod->level;
                payload.lod_error = lod->geometric_error;
                payload.hlod_enter_error_pixels = lod->enter_error_pixels;
                payload.hlod_exit_error_pixels = lod->exit_error_pixels;
            }
            if (const auto* parent = registry->try_get<
                    lux::ecs::VisualLodParentComponent>(entity))
            {
                if (!parent->parent.valid() || !registry->all_of<
                        lux::ecs::PersistentEntityIdComponent>(entity))
                    return false;
                payload.parent = uuidWireId(parent->parent.id);
            }

            const auto* own = registry->try_get<
                lux::ecs::PersistentEntityIdComponent>(entity);
            if (!own)
                return true;
            const auto children = children_by_parent.find(own->id().value());
            if (children == children_by_parent.end())
                return true;
            for (const auto child : children->second)
            {
                if (payload.child_count >=
                    lux::render::kMaximumRenderClusterChildren)
                {
                    return false;
                }
                payload.children[payload.child_count++] = uuidWireId(
                    registry->get<const
                        lux::ecs::PersistentEntityIdComponent>(child).id());
            }
            return true;
        }

        void rebuildTopologyIndex()
        {
            children_by_parent.clear();
            const auto children = registry->view<
                const lux::ecs::ClassicMeshBatchComponent,
                const lux::ecs::VisualLodParentComponent,
                const lux::ecs::PersistentEntityIdComponent>();
            children_by_parent.reserve(children.size_hint());
            for (const auto child : children)
            {
                const auto& parent = children.get<const
                    lux::ecs::VisualLodParentComponent>(child).parent;
                if (parent.valid())
                    children_by_parent[parent.id.value()].push_back(child);
            }
        }

        void fail(Entry& entry, ESceneContentRenderFailure failure) noexcept
        {
            entry.preparation.reset();
            entry.pending.reset();
            entry.failure = failure;
            entry.state = ESceneContentRenderState::FAILED;
        }

        void begin(lux::ecs::Entity entity, Entry& entry)
        {
            if (!residency.await || !residency.request)
            {
                fail(entry, ESceneContentRenderFailure::FEATURE_UNAVAILABLE);
                return;
            }
            const auto& component = registry->get<
                lux::ecs::ClassicMeshBatchComponent>(entity);
            const auto& transform = registry->get<
                lux::ecs::ResolvedTransform3DComponent>(entity);
            if (!component.content.valid() ||
                component.content.type.name() !=
                    lux::classic_mesh::kClassicMeshBatchContentTypeName ||
                component.content.schema_version !=
                    lux::classic_mesh::kClassicMeshBatchSchemaVersion ||
                !std::isfinite(component.local_bounds_radius) ||
                component.local_bounds_radius < 0.0f ||
                !component.local_bounds_center.allFinite() ||
                !transform.linear.allFinite() ||
                !lux::math::isFinite(transform.position))
            {
                fail(entry, ESceneContentRenderFailure::INVALID_COMPONENT);
                return;
            }

            auto lease = blobs.resolve(component.content);
            if (!lease)
            {
                fail(entry, ESceneContentRenderFailure::CONTENT_UNAVAILABLE);
                return;
            }
            entry.preparation.emplace(Preparation{
                entry.desired_generation,
                entry.owner_generation,
                std::move(*lease),
                false});
            entry.failure = ESceneContentRenderFailure::NONE;
            entry.state = ESceneContentRenderState::WAITING_CONTENT;
            launchPreparation(entity, entry);
        }

        [[nodiscard]] ESceneContentRenderFailure preparationFailure(
            const lux::async::OperationFailure<SceneGeometryPrepareFailure>&
                value) const noexcept
        {
            if (value.isRuntime())
                return ESceneContentRenderFailure::FEATURE_UNAVAILABLE;
            switch (value.domainError().code)
            {
            case ESceneGeometryPrepareError::CONTENT_MISMATCH:
                return ESceneContentRenderFailure::CONTENT_MISMATCH;
            case ESceneGeometryPrepareError::UNSUPPORTED_CONTENT:
            case ESceneGeometryPrepareError::INVALID_REQUEST:
                return ESceneContentRenderFailure::INVALID_COMPONENT;
            case ESceneGeometryPrepareError::DECODE_FAILED:
                return ESceneContentRenderFailure::DECODE_FAILED;
            case ESceneGeometryPrepareError::SERVICE_CLOSED:
                return ESceneContentRenderFailure::FEATURE_UNAVAILABLE;
            }
            return ESceneContentRenderFailure::DECODE_FAILED;
        }

        void launchPreparation(
            lux::ecs::Entity entity,
            Entry& entry) noexcept
        {
            if (!entry.preparation || entry.preparation->in_flight)
                return;
            if (!preparation_client)
            {
                fail(entry, ESceneContentRenderFailure::FEATURE_UNAVAILABLE);
                return;
            }
            auto& preparing = *entry.preparation;
            const auto owner_generation = preparing.owner_generation;
            const auto desired_generation = preparing.desired_generation;
            preparing.in_flight = true;
            entry.state = ESceneContentRenderState::WAITING_BACKGROUND;
            struct Completion final
            {
                std::weak_ptr<CallbackControl> callbacks;
                lux::ecs::Entity entity{entt::null};
                std::uint64_t owner_generation{0u};
                std::uint64_t desired_generation{0u};
            };
            auto* completion = new Completion{
                callbacks,
                entity,
                owner_generation,
                desired_generation};
            (void)preparation_client.submit(
                PrepareClassicMeshBatch{
                    preparing.blob.bytes(),
                    preparing.blob.reference(),
                    desired_generation},
                completion,
                +[](void* opaque,
                    lux::async::OperationOutcome<PrepareClassicMeshBatch>&&
                        outcome) noexcept
                {
                    std::unique_ptr<Completion> state{
                        static_cast<Completion*>(opaque)};
                    const auto locked = state->callbacks.lock();
                    if (locked && locked->owner)
                    {
                        locked->owner->impl_->acceptPreparation(
                            state->entity,
                            state->owner_generation,
                            state->desired_generation,
                            std::move(outcome));
                    }
                });
        }

        void acceptPreparation(
            lux::ecs::Entity entity,
            std::uint64_t owner_generation,
            std::uint64_t desired_generation,
            lux::async::OperationOutcome<PrepareClassicMeshBatch> outcome)
            noexcept
        {
            const auto found = entries.find(entity);
            const bool owner_matches = found != entries.end() &&
                found->second.owner_generation == owner_generation;
            const bool request_matches = owner_matches &&
                found->second.preparation &&
                found->second.preparation->desired_generation ==
                    desired_generation;
            const bool desired_matches = request_matches &&
                found->second.desired_generation == desired_generation;
            const auto disposition = detail::classifyContentPreparation(
                owner_matches, request_matches, desired_matches);
            if (disposition == detail::
                    EContentPreparationDisposition::DISCARD_STALE)
            {
                ++metrics.stale_preparation_completions;
                return;
            }
            auto& entry = found->second;
            if (disposition == detail::
                    EContentPreparationDisposition::RETRY_LATEST)
            {
                entry.preparation.reset();
                dirty.insert(entity);
                ++metrics.stale_preparation_completions;
                return;
            }
            if (!outcome)
            {
                if (outcome.error().isRuntime() &&
                    (outcome.error().runtimeError() ==
                         lux::async::ESubmitError::QUEUE_FULL ||
                     outcome.error().runtimeError() ==
                         lux::async::ESubmitError::
                             BYTE_BUDGET_EXHAUSTED))
                {
                    entry.preparation->in_flight = false;
                    entry.state = ESceneContentRenderState::WAITING_CONTENT;
                    ++metrics.preparation_backpressure;
                    return;
                }
                fail(entry, preparationFailure(outcome.error()));
                return;
            }
            auto prepared = std::move(*outcome);
            if (prepared.request_generation != desired_generation ||
                !prepared.decoded || !prepared.wire ||
                prepared.decoded->instances.empty() ||
                prepared.decoded->instances.size() !=
                    prepared.wire->size() ||
                prepared.decoded->instances.size() >
                    std::numeric_limits<std::uint32_t>::max())
            {
                fail(entry, ESceneContentRenderFailure::DECODE_FAILED);
                return;
            }
            if (!registry || !registry->valid(entity) ||
                !registry->all_of<
                    lux::ecs::ClassicMeshBatchComponent,
                    lux::ecs::ResolvedTransform3DComponent>(entity))
            {
                retire(entity);
                return;
            }

            const auto& component = registry->get<
                lux::ecs::ClassicMeshBatchComponent>(entity);
            const auto& transform = registry->get<
                lux::ecs::ResolvedTransform3DComponent>(entity);
            Pending pending;
            pending.desired_generation = desired_generation;
            pending.owner_generation = owner_generation;
            pending.id = entityWireId(*registry, entity);
            pending.payload.scene_id = callbacks->scene;
            pending.payload.id = pending.id;
            pending.payload.instance_count = static_cast<std::uint32_t>(
                prepared.decoded->instances.size());
            pending.payload.transition_milliseconds = 350u;
            const auto center = lux::math::Position3d{
                transform.position.x + static_cast<double>((
                    transform.linear * component.local_bounds_center).x()),
                transform.position.y + static_cast<double>((
                    transform.linear * component.local_bounds_center).y()),
                transform.position.z + static_cast<double>((
                    transform.linear * component.local_bounds_center).z())};
            const auto bounds = lux::ecs::makeRenderLargePosition(
                center, scene_origin);
            if (!bounds || !fillTopology(entity, pending.payload))
            {
                fail(entry, ESceneContentRenderFailure::INVALID_LOD_TOPOLOGY);
                return;
            }
            pending.payload.bounds_center = *bounds;
            float maximum_scale = 0.0f;
            for (Eigen::Index column = 0; column != 3; ++column)
            {
                maximum_scale = std::max(
                    maximum_scale, transform.linear.col(column).norm());
            }
            pending.payload.bounds_radius =
                component.local_bounds_radius * maximum_scale;
            pending.blob = std::move(entry.preparation->blob);
            pending.decoded = std::move(prepared.decoded);
            pending.wire = std::move(prepared.wire);
            pending.assets.reserve(
                prepared.mesh_assets.size() +
                prepared.material_assets.size());
            for (const auto& id : prepared.mesh_assets)
            {
                AssetNeed need;
                need.id = id;
                need.domain = lux::ecs::EResourceDomain::MESH;
                need.reference = assets->acquire(id);
                pending.assets.push_back(std::move(need));
            }
            for (const auto& id : prepared.material_assets)
            {
                AssetNeed need;
                need.id = id;
                need.domain = lux::ecs::materialDomainOf(*assets, id);
                need.reference = assets->acquire(id);
                pending.assets.push_back(std::move(need));
            }
            entry.preparation.reset();
            entry.pending.emplace(std::move(pending));
            entry.failure = ESceneContentRenderFailure::NONE;
            entry.state = entry.pending->assets.empty()
                ? ESceneContentRenderState::WAITING_CONTENT
                : ESceneContentRenderState::WAITING_ASSETS;

            std::vector<std::pair<
                lux::asset::asset_id_t,
                lux::ecs::EResourceDomain>> requests;
            requests.reserve(entry.pending->assets.size());
            for (auto& need : entry.pending->assets)
            {
                const auto id = need.id;
                const auto domain = need.domain;
                need.wait = residency.await(
                    id,
                    [control = std::weak_ptr<CallbackControl>{callbacks},
                     entity,
                     owner_generation,
                     desired_generation,
                     id,
                     domain](
                        std::uint64_t bits,
                        const lux::ecs::ResourceFailure* failure)
                    {
                        if (auto locked = control.lock();
                            locked && locked->owner)
                        {
                            locked->owner->impl_->assetDelivered(
                                entity,
                                owner_generation,
                                desired_generation,
                                id,
                                domain,
                                bits,
                                failure != nullptr);
                        }
                    });
                requests.emplace_back(id, domain);
            }
            // request() may synchronously deliver a READY or terminal result.
            // Iterate an owning copy so a terminal callback may reset pending
            // without invalidating the loop that invoked it.
            for (const auto& [id, domain] : requests)
            {
                if (!entry.pending ||
                    entry.pending->desired_generation != desired_generation)
                {
                    break;
                }
                residency.request(id, domain);
            }
        }

        void assetDelivered(
            lux::ecs::Entity entity,
            std::uint64_t owner_generation,
            std::uint64_t desired_generation,
            const lux::asset::asset_id_t& id,
            lux::ecs::EResourceDomain domain,
            std::uint64_t bits,
            bool delivery_failed) noexcept
        {
            const auto found = entries.find(entity);
            if (found == entries.end() ||
                found->second.owner_generation != owner_generation ||
                !found->second.pending ||
                found->second.pending->desired_generation !=
                    desired_generation)
            {
                return;
            }
            auto& entry = found->second;
            auto& pending = *entry.pending;
            const auto need = std::find_if(
                pending.assets.begin(), pending.assets.end(),
                [&](const AssetNeed& candidate)
                {
                    return sameNeed(
                        candidate.id,
                        candidate.domain,
                        id,
                        domain);
                });
            if (need == pending.assets.end())
                return;
            need->wait.reset();
            need->failed = delivery_failed || bits == 0u;
            need->ready = !need->failed;
            need->handle_bits = bits;
            if (need->failed)
            {
                fail(entry, ESceneContentRenderFailure::ASSET_UNAVAILABLE);
                return;
            }
            if (std::ranges::all_of(
                    pending.assets,
                    [](const AssetNeed& value) { return value.ready; }))
            {
                entry.state = ESceneContentRenderState::WAITING_CONTENT;
            }
        }

        [[nodiscard]] const AssetNeed* findAsset(
            const Pending& pending,
            const lux::asset::asset_id_t& id,
            lux::ecs::EResourceDomain domain) const noexcept
        {
            const auto found = std::find_if(
                pending.assets.begin(), pending.assets.end(),
                [&](const AssetNeed& candidate)
                {
                    return sameNeed(
                        candidate.id, candidate.domain, id, domain);
                });
            return found == pending.assets.end() ? nullptr : &*found;
        }

        [[nodiscard]] bool buildRows(
            lux::ecs::Entity entity,
            Entry& entry)
        {
            auto& pending = *entry.pending;
            const auto& transform = registry->get<
                lux::ecs::ResolvedTransform3DComponent>(entity);
            const auto end = std::min(
                pending.next_row + kRowsPerUpdate,
                pending.decoded->instances.size());
            for (; pending.next_row != end; ++pending.next_row)
            {
                const auto& source = pending.decoded->instances[
                    pending.next_row];
                const auto* mesh = findAsset(
                    pending,
                    source.mesh_asset,
                    lux::ecs::EResourceDomain::MESH);
                const auto material_domain = source.material_asset.is_nil()
                    ? lux::ecs::EResourceDomain::MATERIAL
                    : lux::ecs::materialDomainOf(
                          *assets, source.material_asset);
                const auto* material = source.material_asset.is_nil()
                    ? nullptr
                    : findAsset(
                          pending, source.material_asset, material_domain);
                if (!mesh || !mesh->ready ||
                    (!source.material_asset.is_nil() &&
                        (!material || !material->ready)))
                {
                    return false;
                }

                Eigen::Quaternionf rotation{
                    source.rotation[3],
                    source.rotation[0],
                    source.rotation[1],
                    source.rotation[2]};
                const Eigen::Vector3f local_translation{
                    source.translation[0],
                    source.translation[1],
                    source.translation[2]};
                const Eigen::Vector3f local_scale{
                    source.scale[0], source.scale[1], source.scale[2]};
                const Eigen::Matrix3f basis = transform.linear *
                    rotation.toRotationMatrix() * local_scale.asDiagonal();
                const Eigen::Vector3f translated =
                    transform.linear * local_translation;
                const auto position = lux::math::Position3d{
                    transform.position.x + translated.x(),
                    transform.position.y + translated.y(),
                    transform.position.z + translated.z()};
                const auto large = lux::ecs::makeRenderLargePosition(
                    position, scene_origin);
                if (!large || !basis.allFinite())
                    return false;

                auto& target = (*pending.wire)[pending.next_row];
                target.transform.page_delta[0] = large->page_delta[0];
                target.transform.page_delta[1] = large->page_delta[1];
                target.transform.page_delta[2] = large->page_delta[2];
                for (std::size_t column = 0u; column != 3u; ++column)
                {
                    target.transform.basis_local[column * 4u + 0u] =
                        basis(0, static_cast<Eigen::Index>(column));
                    target.transform.basis_local[column * 4u + 1u] =
                        basis(1, static_cast<Eigen::Index>(column));
                    target.transform.basis_local[column * 4u + 2u] =
                        basis(2, static_cast<Eigen::Index>(column));
                }
                target.transform.basis_local[3] = large->local[0];
                target.transform.basis_local[7] = large->local[1];
                target.transform.basis_local[11] = large->local[2];
                target.mesh = lux::ecs::unpackHandleBits<
                    lux::render::RMeshHandle>(mesh->handle_bits);
                if (material)
                {
                    target.material = lux::ecs::unpackHandleBits<
                        lux::render::RMaterialHandle>(
                            material->handle_bits);
                }
                target.stable_pick_id = source.stable_pick_id;
                target.rgba8 = source.rgba8;
                target.flags = renderFlags(source.flags);
            }
            return true;
        }
