    namespace
    {
        constexpr float kPi = 3.14159265358979323846f;

        /// Translate orbit state into a TRS that places the camera at
        /// `target + R(yaw,pitch) * (0, 0, distance)` looking along its
        /// local -Z towards `target`. Used once at bringUp to seed the camera's
        /// initial pose; `CameraSceneSystem`'s `EditorCamera3DController` then drives
        /// it live from input. Mirrors the controller's TRS convention.
        void writeOrbitToTransform(const OrbitCameraState& o, lux::ecs::Transform3DComponent& tc)
        {
            const float yaw_r   = o.yaw_deg   * kPi / 180.f;
            const float pitch_r = o.pitch_deg * kPi / 180.f;

            const Eigen::Quaternionf q_yaw(
                Eigen::AngleAxisf(yaw_r, Eigen::Vector3f::UnitY()));
            const Eigen::Quaternionf q_pitch(
                Eigen::AngleAxisf(pitch_r, Eigen::Vector3f::UnitX()));

            tc.rotation = (q_yaw * q_pitch).normalized();
            const Eigen::Vector3f back =
                tc.rotation * Eigen::Vector3f(0.f, 0.f, o.distance);
            tc.position = {
                static_cast<double>(o.target_x + back.x()),
                static_cast<double>(o.target_y + back.y()),
                static_cast<double>(o.target_z + back.z())};
        }

        [[nodiscard]] std::uint32_t fileMagic(
            const std::filesystem::path& path) noexcept
        {
            std::ifstream stream(path, std::ios::binary);
            std::uint32_t magic = 0u;
            if (stream)
                stream.read(reinterpret_cast<char*>(&magic), sizeof(magic));
            return stream ? magic : 0u;
        }

        [[nodiscard]] std::size_t descriptorPageResidentBytes(
            const lux::authoring::WorldDescriptorPageDocument& page)
            noexcept
        {
            std::size_t bytes = sizeof(page) +
                page.actors.capacity() *
                    sizeof(lux::authoring::WorldActorSourceDescriptor) +
                page.pages.capacity() *
                    sizeof(lux::authoring::WorldPageSourceDescriptor);
            for (const auto& actor : page.actors)
            {
                bytes += actor.display_name.capacity() +
                    actor.actor_class.capacity() +
                    actor.document_path.capacity() +
                    actor.data_layers.capacity() *
                        sizeof(lux::authoring::DataLayerId) +
                    actor.references.capacity() *
                        sizeof(lux::authoring::WorldActorSourceReference);
                for (const auto& layer : actor.data_layers)
                    bytes += layer.name().capacity();
            }
            for (const auto& content : page.pages)
                bytes += content.document_path.capacity();
            return bytes;
        }

        lux::math::AABB computeLocalAABB(
            const lux::rdesc::Mesh& mesh);

        [[nodiscard]] std::optional<std::vector<std::byte>> readDocumentBytes(
            const std::filesystem::path& path)
        {
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            if (!stream)
                return std::nullopt;
            const auto end = stream.tellg();
            if (end < 0)
                return std::nullopt;
            std::vector<std::byte> bytes(static_cast<std::size_t>(end));
            stream.seekg(0);
            if (!bytes.empty() && !stream.read(
                    reinterpret_cast<char*>(bytes.data()), end))
            {
                return std::nullopt;
            }
            return bytes;
        }

        [[nodiscard]] std::optional<lux::authoring::WorldCellKey>
        descriptorCell(
            const lux::authoring::WorldSourceDocument& root,
            lux::authoring::PartitionSpaceId space_id,
            const lux::authoring::WorldActorSourcePosition& position)
        {
            const auto space = std::ranges::find(
                root.spaces, space_id, [](const auto& value)
                {
                    return value.id;
                });
            if (space == root.spaces.end())
                return std::nullopt;
            const auto coordinate = [&](double value)
                -> std::optional<std::int64_t>
            {
                if (!std::isfinite(value) ||
                    !lux::authoring::isValidCellEdge(space->cell_edge))
                    return std::nullopt;
                const auto result = std::floor(
                    value / static_cast<double>(space->cell_edge));
                if (result < static_cast<double>(
                        std::numeric_limits<std::int64_t>::min()) ||
                    result > static_cast<double>(
                        std::numeric_limits<std::int64_t>::max()))
                    return std::nullopt;
                return static_cast<std::int64_t>(result);
            };
            lux::authoring::WorldCellKey cell;
            cell.topology = space->topology;
            if (space->topology ==
                lux::authoring::EPartitionTopology::PLANAR_XY)
            {
                const auto* point = std::get_if<
                    lux::spatial::Position2D>(&position);
                if (!point)
                    return std::nullopt;
                const auto x = coordinate(point->x);
                const auto y = coordinate(point->y);
                if (!x || !y)
                    return std::nullopt;
                cell.coordinate = lux::authoring::PlanarCellCoord{*x, *y};
            }
            else
            {
                const auto* point = std::get_if<
                    lux::spatial::Position3D>(&position);
                if (!point)
                    return std::nullopt;
                if (space->topology ==
                    lux::authoring::EPartitionTopology::PLANAR_XZ)
                {
                    const auto x = coordinate(point->x);
                    const auto z = coordinate(point->z);
                    if (!x || !z)
                        return std::nullopt;
                    cell.coordinate = lux::authoring::PlanarCellCoord{*x, *z};
                }
                else
                {
                    const auto x = coordinate(point->x);
                    const auto y = coordinate(point->y);
                    const auto z = coordinate(point->z);
                    if (!x || !y || !z)
                        return std::nullopt;
                    cell.coordinate = lux::authoring::VolumeCellCoord{
                        *x, *y, *z};
                }
            }
            return cell;
        }

        [[nodiscard]] std::optional<lux::authoring::WorldMacroCoord>
        descriptorMacro(
            const lux::authoring::WorldSourceDocument& root,
            lux::authoring::PartitionSpaceId space_id,
            const lux::authoring::WorldActorSourcePosition& position)
        {
            const auto cell = descriptorCell(root, space_id, position);
            if (!cell)
                return std::nullopt;
            const auto space = std::ranges::find(
                root.spaces, space_id, [](const auto& value)
                {
                    return value.id;
                });
            if (space == root.spaces.end())
                return std::nullopt;
            return lux::authoring::macroCoordOf(
                *cell, space->macro_edge_cells);
        }

        [[nodiscard]] std::string terrainPageKey(
            lux::authoring::TerrainSetId terrain,
            const lux::authoring::WorldCellKey& cell)
        {
            auto result = uuids::to_string(terrain.value());
            result += '/' + std::to_string(
                static_cast<std::uint8_t>(cell.topology));
            if (const auto* planar = std::get_if<
                    lux::authoring::PlanarCellCoord>(&cell.coordinate))
            {
                result += '/' + std::to_string(planar->a);
                result += '/' + std::to_string(planar->b);
            }
            else if (const auto* volume = std::get_if<
                         lux::authoring::VolumeCellCoord>(&cell.coordinate))
            {
                result += '/' + std::to_string(volume->x);
                result += '/' + std::to_string(volume->y);
                result += '/' + std::to_string(volume->z);
            }
            return result;
        }

        [[nodiscard]] lux::entity_scene::EntitySceneManifest runtimeManifest(
            const lux::authoring::WorldSourceDocument& source)
        {
            lux::entity_scene::EntitySceneManifest manifest;
            manifest.id = lux::entity_scene::EntitySceneId{
                source.world.value()};
            manifest.contributions = source.contributions;
            manifest.required_extensions = source.required_extensions;
            return manifest;
        }

        /// Early LXWA v4 editor/demo writers emitted roots with an empty
        /// contribution plan. Such a scene has neither Camera2DSystem nor
        /// Camera3DSystem and its editor viewport remains black. Repair that
        /// precise legacy shape at the Authoring boundary so the next save
        /// persists the inferred presentation contribution.
        [[nodiscard]] bool ensurePresentationContribution(
            lux::authoring::WorldSourceDocument& source)
        {
            if (!source.contributions.empty() || source.spaces.empty())
                return false;

            const bool is_2d =
                source.spaces.front().topology ==
                lux::authoring::EPartitionTopology::PLANAR_XY;
            source.contributions.push_back({
                lux::extensions::ContributionId{
                    is_2d
                        ? "org.lux.builtin.presentation2d"
                        : "org.lux.builtin.presentation3d"},
                0u,
                {}});
            return true;
        }

        struct PlayEntitySceneImage final
        {
            lux::asset::AssetBlob manifest;
            std::shared_ptr<const lux::asset::AssetVfs> vfs;
        };

        [[nodiscard]] lux::cxx::expected<PlayEntitySceneImage, std::string>
        makePlayEntitySceneImage(
            const std::filesystem::path& pak_file,
            lux::entity_scene::EntitySceneId scene_id) noexcept
        {
            auto pak = lux::asset::PakAssetProvider::loadFromFile(pak_file);
            if (!pak)
                return lux::cxx::unexpected(std::move(pak.error()));
            auto selected = lux::asset::resolveBootScene(
                **pak, "Scenes/Play");
            if (!selected || selected->id != scene_id.value())
            {
                return lux::cxx::unexpected(std::string{
                    "cooked Play Pak does not expose the expected "
                    "ENTITY_SCENE entry"});
            }
            auto manifest = (*pak)->open(scene_id.value());
            if (!manifest)
            {
                return lux::cxx::unexpected(
                    std::string{"cannot open cooked Play LXSC manifest"});
            }
            auto vfs = std::make_shared<lux::asset::AssetVfs>();
            if (vfs->mount({"/Game", std::move(*pak), 0}) ==
                lux::asset::kInvalidMountId)
            {
                return lux::cxx::unexpected(
                    std::string{"cannot mount cooked Play EntityScene"});
            }
            return PlayEntitySceneImage{
                std::move(*manifest),
                std::move(vfs)};
        }

    } // namespace

    struct EditorScene::PlayCookControl final
    {
        EditorScene* owner{nullptr};
        std::uint64_t generation{0u};
    };

    // -------------------------------------------------------------------------
    EditorScene::EditorScene(lux::ui::UIRenderFrameSession& session,
                             lux::asset::AssetManager& assets,
                             const EditorRenderInfra&  infra,
                             lux::asset_runtime::AssetClient asset_client,
                             lux::exec::AsyncRuntime& async,
                             EditorAsyncService& editor_async,
                             const lux::ecs::ComponentTypeCatalog& components) noexcept
        : session_(session)
        , assets_(assets)
        , infra_(infra)
        , asset_client_(std::move(asset_client))
        , async_(async)
        , editor_async_(editor_async)
        , components_(components)
    {
        // Scene/view/features are SCENE-domain state owned by the runtime_
        // (constructed in bringUp, destroyed in tearDown). The infra carries
        // only PROCESS-domain state: feature TYPE registrations + the attach
        // plan + imgui ops.
        play_cook_control_ = std::make_shared<PlayCookControl>();
        play_cook_control_->owner = this;
    }

    EditorScene::~EditorScene()
    {
        // 自己收自己的尾(此前是 `= default`)。tearDown() 独占地做几件没有别人会做
        // 的事 —— 归还宿主的 main_target_、关闭独立 Cooked Play Runtime、在 World
        // 还有效时跑脚本的 OnDestroy —— 而这些一旦漏调**不会崩,只会静默泄漏**。
        // tearDown() 是 noexcept 且幂等(`if (!live_) return;`),正常路径
        // SceneController::unloadScene() 已经调过一次。
        (void)tearDown();
    }

    CameraSceneSystem* EditorScene::cameraSystem() noexcept
    {
        return runtime_ ? runtime_->schedule().get(camera_system_) : nullptr;
    }

    const CameraSceneSystem* EditorScene::cameraSystem() const noexcept
    {
        return runtime_ ? runtime_->schedule().get(camera_system_) : nullptr;
    }

    // -------------------------------------------------------------------------
    lux::ecs::SceneSettingsComponent& EditorScene::ensureSceneSettings()
    {
        auto& reg = runtime_->world().registry();
        if (auto v = reg.view<lux::ecs::SceneSettingsComponent>(); v.begin() != v.end())
            return v.get<lux::ecs::SceneSettingsComponent>(*v.begin());

        // None yet (small / new scene): create the scene-settings singleton with
        // struct defaults.
        const auto e = reg.create();
        reg.emplace<lux::ecs::NameComponent>(e,
            lux::ecs::NameComponent{"Scene Settings"});
        return reg.emplace<lux::ecs::SceneSettingsComponent>(e,
            lux::ecs::SceneSettingsComponent{});
    }

    std::size_t EditorScene::indexedWorldActorCount() const noexcept
    {
        return world_descriptor_index_
            ? world_descriptor_index_->actorCount()
            : 0u;
    }

    std::optional<lux::runtime::spatial3d::Physics3DSceneSnapshot>
    EditorScene::physics3DDebugSnapshot() noexcept
    {
        auto* active = play_runtime_ ? play_runtime_.get() : runtime_.get();
        if (!active)
            return std::nullopt;
        const auto* service = active->services().get<
            lux::runtime::spatial3d::Physics3DSceneService>();
        if (!service || !service->scene)
            return std::nullopt;
        return service->snapshot();
    }

    std::optional<lux::navigation::NavigationPathResult>
    EditorScene::queryNavigationPath(
        const lux::navigation::NavigationPathRequest& request) noexcept
    {
        auto* active = play_runtime_ ? play_runtime_.get() : runtime_.get();
        if (!active || !lux::navigation::valid(request))
            return std::nullopt;
        const auto* service = active->services().get<
            lux::ecs::NavigationQueryService>();
        if (!service)
            return std::nullopt;
        return service->query(request);
    }

    void EditorScene::restoreAuthoringViewpoint(
        lux::meta::entity_id source_camera) noexcept
    {
        if (!runtime_ || camera_entity_ == entt::null)
            return;
        auto& registry = runtime_->world().registry();
        if (!registry.valid(source_camera) ||
            !registry.valid(camera_entity_) || source_camera == camera_entity_)
        {
            return;
        }
        if (const auto* source = registry.try_get<
                lux::ecs::Transform3DComponent>(source_camera);
            source && registry.all_of<lux::ecs::Transform3DComponent>(
                camera_entity_))
        {
            registry.patch<lux::ecs::Transform3DComponent>(
                camera_entity_,
                [source](auto& target) { target = *source; });
        }
        if (const auto* source = registry.try_get<
                lux::ecs::Transform2DComponent>(source_camera);
            source && registry.all_of<lux::ecs::Transform2DComponent>(
                camera_entity_))
        {
            registry.patch<lux::ecs::Transform2DComponent>(
                camera_entity_,
                [source](auto& target) { target = *source; });
        }
    }

    std::vector<lux::authoring::WorldDescriptorIndexActor>
    EditorScene::queryWorldActors(
        std::string_view text,
        std::size_t offset,
        std::size_t maximum) const
    {
        return world_descriptor_index_
            ? world_descriptor_index_->search(text, offset, maximum)
            : std::vector<
                  lux::authoring::WorldDescriptorIndexActor>{};
    }

    std::optional<lux::authoring::EditableWorldInstance>
    EditorScene::worldInstance(
        lux::authoring::WorldInstanceId instance) const
    {
        if (!instance.valid())
            return std::nullopt;
        for (const auto& [_, page] : authoring_instance_clusters_)
        {
            const auto found = std::ranges::find(
                page.page.instances,
                instance,
                &lux::authoring::EditableWorldInstance::id);
            if (found != page.page.instances.end())
                return *found;
        }
        return std::nullopt;
    }

    lux::cxx::expected<void, std::string>
    EditorScene::updateWorldInstance(
        lux::authoring::EditableWorldInstance instance)
    {
        instance_edit_error_.clear();
        if (!world_source_ || !instance.id.valid())
        {
            instance_edit_error_ = "No editable World Instance is selected";
            return lux::cxx::unexpected(instance_edit_error_);
        }
        const bool finite_position = std::visit(
            [](const auto& position)
            {
                return lux::spatial::isFinite(position);
            },
            instance.position);
        float rotation_norm = 0.0f;
        for (const auto value : instance.rotation)
            rotation_norm += value * value;
        if (!finite_position || !std::isfinite(rotation_norm) ||
            rotation_norm <= 1.0e-12f)
        {
            instance_edit_error_ = "Instance transform is not finite";
            return lux::cxx::unexpected(instance_edit_error_);
        }
        const auto inverse_rotation_norm = 1.0f / std::sqrt(rotation_norm);
        for (auto& value : instance.rotation)
            value *= inverse_rotation_norm;

        auto source = authoring_instance_clusters_.end();
        for (auto candidate = authoring_instance_clusters_.begin();
             candidate != authoring_instance_clusters_.end(); ++candidate)
        {
            if (std::ranges::find(
                    candidate->second.page.instances,
                    instance.id,
                    &lux::authoring::EditableWorldInstance::id) !=
                candidate->second.page.instances.end())
            {
                source = candidate;
                break;
            }
        }
        if (source == authoring_instance_clusters_.end())
        {
            instance_edit_error_ = "Instance Page is not materialized";
            return lux::cxx::unexpected(instance_edit_error_);
        }
        const auto cell = descriptorCell(
            *world_source_,
            source->second.page.space,
            instance.position);
        if (!cell)
        {
            instance_edit_error_ = "Instance position is invalid for its Space";
            return lux::cxx::unexpected(instance_edit_error_);
        }

        auto destination = source;
        if (*cell != source->second.page.cell)
        {
            destination = std::ranges::find_if(
                authoring_instance_clusters_,
                [&](const auto& entry)
                {
                    return entry.second.page.instance_set ==
                            source->second.page.instance_set &&
                        entry.second.page.space == source->second.page.space &&
                        entry.second.page.cell == *cell;
                });
            if (destination == authoring_instance_clusters_.end())
            {
                instance_edit_error_ =
                    "Destination Instance Page is not materialized; move the viewport or pin that Cell first";
                return lux::cxx::unexpected(instance_edit_error_);
            }
        }

        auto moved = lux::authoring::moveWorldInstance(
            *world_source_,
            source->second.page,
            destination->second.page,
            instance.id,
            instance.position);
        if (!moved)
        {
            instance_edit_error_ = moved.error().detail;
            return lux::cxx::unexpected(instance_edit_error_);
        }
        auto replaceInstance = [&](auto& page)
        {
            const auto found = std::ranges::find(
                page.instances,
                instance.id,
                &lux::authoring::EditableWorldInstance::id);
            if (found != page.instances.end())
                *found = instance;
        };
        if (moved->destination_page)
            replaceInstance(*moved->destination_page);
        else
            replaceInstance(moved->source_page);

        if (!lux::authoring::encodeWorldInstancePage(
                *world_source_, moved->source_page) ||
            (moved->destination_page &&
             !lux::authoring::encodeWorldInstancePage(
                 *world_source_, *moved->destination_page)))
        {
            instance_edit_error_ = "Edited Instance Page failed validation";
            return lux::cxx::unexpected(instance_edit_error_);
        }

        const auto source_before = source->second;
        const auto source_key = source->first;
        if (!activateAuthoringInstancePage(
                source_before.descriptor_page,
                source_before.descriptor,
                std::move(moved->source_page)))
        {
            instance_edit_error_ = "Authoring Instance Page rejected the edit";
            return lux::cxx::unexpected(instance_edit_error_);
        }
        dirty_instance_pages_.insert(source_key);
        if (moved->destination_page)
        {
            const auto destination_before = destination->second;
            const auto destination_key = destination->first;
            if (!activateAuthoringInstancePage(
                    destination_before.descriptor_page,
                    destination_before.descriptor,
                    std::move(*moved->destination_page)))
            {
                const bool restored = activateAuthoringInstancePage(
                    source_before.descriptor_page,
                    source_before.descriptor,
                    source_before.page);
                dirty_instance_pages_.erase(source_key);
                instance_edit_error_ = restored
                    ? "Destination Authoring Page rejected the cross-Cell move"
                    : "Cross-Cell move rollback failed";
                return lux::cxx::unexpected(instance_edit_error_);
            }
            dirty_instance_pages_.insert(destination_key);
        }
        return {};
    }

    lux::cxx::expected<lux::authoring::WorldInstanceId, std::string>
    EditorScene::duplicateWorldInstance(
        lux::authoring::WorldInstanceId instance)
    {
        instance_edit_error_.clear();
        if (!world_source_)
        {
            instance_edit_error_ = "No Authoring World is open";
            return lux::cxx::unexpected(instance_edit_error_);
        }
        for (auto& [key, page] : authoring_instance_clusters_)
        {
            const auto found = std::ranges::find(
                page.page.instances,
                instance,
                &lux::authoring::EditableWorldInstance::id);
            if (found == page.page.instances.end())
                continue;
            auto duplicated = lux::authoring::duplicateWorldInstance(
                *world_source_, page.page, instance, found->position);
            if (!duplicated)
            {
                instance_edit_error_ = duplicated.error().detail;
                return lux::cxx::unexpected(instance_edit_error_);
            }
            if (!activateAuthoringInstancePage(
                    page.descriptor_page,
                    page.descriptor,
                    std::move(duplicated->page)))
            {
                instance_edit_error_ =
                    "Authoring Instance Page rejected the duplicate";
                return lux::cxx::unexpected(instance_edit_error_);
            }
            const auto allocator = std::ranges::find(
                world_source_->instance_sets,
                duplicated->instance_set.id,
                &lux::authoring::WorldInstanceSetSourceDescriptor::id);
            if (allocator == world_source_->instance_sets.end())
            {
                std::abort();
            }
            allocator->next_local_id =
                duplicated->instance_set.next_local_id;
            dirty_instance_pages_.insert(key);
            selection_.selectObject(EditorObjectId{
                duplicated->created_instance.id});
            return duplicated->created_instance.id;
        }
        instance_edit_error_ = "Instance Page is not materialized";
        return lux::cxx::unexpected(instance_edit_error_);
    }

    lux::cxx::expected<void, std::string>
    EditorScene::deleteWorldInstance(
        lux::authoring::WorldInstanceId instance)
    {
        instance_edit_error_.clear();
        for (auto& [key, page] : authoring_instance_clusters_)
        {
            const auto found = std::ranges::find(
                page.page.instances,
                instance,
                &lux::authoring::EditableWorldInstance::id);
            if (found == page.page.instances.end())
                continue;
            auto removed = lux::authoring::deleteWorldInstance(
                page.page, instance);
            if (!removed)
            {
                instance_edit_error_ = removed.error().detail;
                return lux::cxx::unexpected(instance_edit_error_);
            }
            if (!activateAuthoringInstancePage(
                    page.descriptor_page,
                    page.descriptor,
                    std::move(removed->page)))
            {
                instance_edit_error_ =
                    "Authoring Instance Page rejected the deletion";
                return lux::cxx::unexpected(instance_edit_error_);
            }
            dirty_instance_pages_.insert(key);
            if (selection_.object() &&
                *selection_.object() == EditorObjectId{instance})
            {
                selection_.clear();
            }
            return {};
        }
        instance_edit_error_ = "Instance Page is not materialized";
        return lux::cxx::unexpected(instance_edit_error_);
    }

    lux::cxx::expected<lux::entity_scene::PersistentEntityId, std::string>
    EditorScene::convertWorldInstanceToActor(
        lux::authoring::WorldInstanceId instance_id)
    {
        instance_edit_error_.clear();
        if (!world_source_)
        {
            instance_edit_error_ =
                "Instance-to-Actor conversion requires an editable scene";
            return lux::cxx::unexpected(instance_edit_error_);
        }
        auto instance_page = authoring_instance_clusters_.end();
        for (auto candidate = authoring_instance_clusters_.begin();
             candidate != authoring_instance_clusters_.end(); ++candidate)
        {
            if (std::ranges::find(
                    candidate->second.page.instances,
                    instance_id,
                    &lux::authoring::EditableWorldInstance::id) !=
                candidate->second.page.instances.end())
            {
                instance_page = candidate;
                break;
            }
        }
        if (instance_page == authoring_instance_clusters_.end())
        {
            instance_edit_error_ = "Instance Page is not materialized";
            return lux::cxx::unexpected(instance_edit_error_);
        }
        auto* descriptor_page = cachedAuthoringDescriptorPage(
            instance_page->second.descriptor_page);
        if (!descriptor_page)
        {
            instance_edit_error_ =
                "Actor Descriptor Page is not resident; keep the Cell in the edit window and retry";
            return lux::cxx::unexpected(instance_edit_error_);
        }
        const auto source = std::ranges::find(
            instance_page->second.page.instances,
            instance_id,
            &lux::authoring::EditableWorldInstance::id);
        if (source == instance_page->second.page.instances.end() ||
            !std::holds_alternative<lux::spatial::Position3D>(
                source->position))
        {
            instance_edit_error_ =
                "Instance-to-Actor conversion requires a 3D Instance";
            return lux::cxx::unexpected(instance_edit_error_);
        }

        lux::meta::EntityRegistry staging;
        const auto staging_entity = staging.create();
        staging.emplace<lux::ecs::NameComponent>(
            staging_entity,
            lux::ecs::NameComponent{
                "Instance " + std::to_string(instance_id.local_id)});
        lux::ecs::Transform3DComponent transform;
        transform.position = std::get<lux::spatial::Position3D>(
            source->position);
        transform.rotation = Eigen::Quaternionf{
            source->rotation[3],
            source->rotation[0],
            source->rotation[1],
            source->rotation[2]}.normalized();
        transform.scale = {
            source->scale[0],
            source->scale[1],
            source->scale[2]};
        staging.emplace<lux::ecs::Transform3DComponent>(
            staging_entity, transform);
        lux::ecs::MeshComponent mesh;
        mesh.mesh_asset_id = source->mesh;
        mesh.material_asset_id = source->material_instance;
        staging.emplace<lux::ecs::MeshComponent>(staging_entity, mesh);

        lux::ecs::PersistentEntityIndex staging_persistent_entities{
            staging};
        WorldActorEcsAdapter adapter{
            components_,
            staging_persistent_entities};
        auto actor_document = adapter.capture(
            staging,
            staging_entity,
            world_source_->world,
            "Instance-to-Actor staging");
        if (!actor_document)
        {
            instance_edit_error_ = actor_document.error();
            return lux::cxx::unexpected(instance_edit_error_);
        }
        lux::authoring::WorldActorSourceDescriptor actor_descriptor;
        actor_descriptor.id = actor_document->actor;
        actor_descriptor.display_name =
            "Instance " + std::to_string(instance_id.local_id);
        actor_descriptor.actor_class = "org.lux.static_instance_actor";
        actor_descriptor.space = instance_page->second.page.space;
        actor_descriptor.position = source->position;
        actor_descriptor.bounds_half_extent = {
            std::abs(source->scale[0]),
            std::abs(source->scale[1]),
            std::abs(source->scale[2])};
        actor_descriptor.data_layers = source->data_layers;

        auto transaction = lux::authoring::convertInstanceToActor(
            *world_source_,
            instance_page->second.page,
            *descriptor_page,
            instance_id,
            actor_descriptor,
            std::move(*actor_document));
        if (!transaction)
        {
            instance_edit_error_ = transaction.error().detail;
            return lux::cxx::unexpected(instance_edit_error_);
        }
        auto encoded_actor = lux::authoring::encodeWorldActorDocument(
            transaction->actor_document);
        if (!encoded_actor)
        {
            instance_edit_error_ = encoded_actor.error();
            return lux::cxx::unexpected(instance_edit_error_);
        }
        const auto actor_digest = lux::cxx::algorithm::Sha256::hash(*encoded_actor);
        const auto actor_path =
            lux::authoring::makeWorldActorDocumentPath(
                transaction->actor_document.actor,
                actor_digest);
        transaction->actor_descriptor.document_path = actor_path;
        transaction->actor_descriptor.content_digest = actor_digest;
        const auto descriptor_entry = std::ranges::find(
            transaction->actor_descriptor_page.actors,
            transaction->actor_document.actor,
            &lux::authoring::WorldActorSourceDescriptor::id);
        if (descriptor_entry ==
            transaction->actor_descriptor_page.actors.end())
        {
            std::abort();
        }
        descriptor_entry->document_path = actor_path;
        descriptor_entry->content_digest = actor_digest;
        if (!lux::authoring::encodeWorldInstancePage(
                *world_source_, transaction->instance_page) ||
            !lux::authoring::encodeWorldDescriptorPage(
                *world_source_, transaction->actor_descriptor_page) ||
            !encoded_actor)
        {
            instance_edit_error_ =
                "Instance-to-Actor transaction failed document validation";
            return lux::cxx::unexpected(instance_edit_error_);
        }

        auto& registry = runtime_->world().registry();
        auto entity = adapter.materialize(
            transaction->actor_document,
            registry,
            "converted Instance");
        if (!entity)
        {
            instance_edit_error_ = entity.error();
            return lux::cxx::unexpected(instance_edit_error_);
        }
        registry.emplace_or_replace<lux::ecs::ResolvedTransform3DComponent>(
            *entity);

        const auto page_before = instance_page->second;
        if (!activateAuthoringInstancePage(
                page_before.descriptor_page,
                page_before.descriptor,
                transaction->instance_page))
        {
            registry.destroy(*entity);
            instance_edit_error_ =
                "Authoring Instance Page rejected Instance-to-Actor conversion";
            return lux::cxx::unexpected(instance_edit_error_);
        }
        cacheAuthoringDescriptorPage(
            transaction->actor_descriptor_page);
        const auto actor_key = uuids::to_string(
            transaction->actor_document.actor.value());
        materialized_actor_ids_.insert(actor_key);
        materialized_actor_descriptors_.insert_or_assign(
            actor_key,
            transaction->actor_descriptor);
        dirty_actor_ids_.insert(actor_key);
        dirty_instance_pages_.insert(instance_page->first);
        authoring_load_result_.created_entities.push_back(*entity);
        authoring_load_result_.world_entity_ids.push_back(
            lux::entity_scene::PersistentEntityId{
                transaction->actor_document.actor.value()});
        selection_.select(&registry, *entity);
        trimAuthoringDescriptorPageCache();
        return lux::entity_scene::PersistentEntityId{
            transaction->actor_document.actor.value()};
    }

    lux::cxx::expected<lux::authoring::WorldInstanceId, std::string>
    EditorScene::convertWorldActorToInstance(
        lux::entity_scene::PersistentEntityId actor)
    {
        instance_edit_error_.clear();
        if (!world_source_ || actor.empty())
        {
            instance_edit_error_ = "No editable World Actor is selected";
            return lux::cxx::unexpected(instance_edit_error_);
        }
        const auto actor_key = uuids::to_string(actor.value());
        const auto source_descriptor = materialized_actor_descriptors_.find(
            actor_key);
        if (source_descriptor == materialized_actor_descriptors_.end())
        {
            instance_edit_error_ = "Actor proxy is not materialized";
            return lux::cxx::unexpected(instance_edit_error_);
        }
        const auto macro = descriptorMacro(
            *world_source_,
            source_descriptor->second.space,
            source_descriptor->second.position);
        if (!macro)
        {
            instance_edit_error_ = "Actor position is invalid for its Space";
            return lux::cxx::unexpected(instance_edit_error_);
        }
        const auto descriptor_page_id =
            lux::authoring::makeWorldDescriptorPageId(
                world_source_->world,
                source_descriptor->second.space,
                *macro);
        auto* descriptor_page = cachedAuthoringDescriptorPage(
            descriptor_page_id);
        if (!descriptor_page)
        {
            instance_edit_error_ =
                "Actor Descriptor Page is not resident; keep the Actor pinned and retry";
            return lux::cxx::unexpected(instance_edit_error_);
        }

        std::size_t actor_index = authoring_load_result_.world_entity_ids.size();
        for (std::size_t index = 0u;
             index < authoring_load_result_.world_entity_ids.size(); ++index)
        {
            if (authoring_load_result_.world_entity_ids[index] == actor)
            {
                actor_index = index;
                break;
            }
        }
        auto& registry = runtime_->world().registry();
        if (actor_index >= authoring_load_result_.created_entities.size() ||
            !registry.valid(
                authoring_load_result_.created_entities[actor_index]))
        {
            instance_edit_error_ = "Actor ECS proxy is unavailable";
            return lux::cxx::unexpected(instance_edit_error_);
        }
        const auto entity = authoring_load_result_.created_entities[actor_index];
        const auto* transform = registry.try_get<
            lux::ecs::Transform3DComponent>(entity);
        const auto* mesh = registry.try_get<lux::ecs::MeshComponent>(entity);
        if (!transform || !mesh || mesh->mesh_asset_id.is_nil())
        {
            instance_edit_error_ =
                "Actor-to-Instance requires one static Mesh and Transform3D";
            return lux::cxx::unexpected(instance_edit_error_);
        }

        const auto actor_cell = descriptorCell(
            *world_source_,
            source_descriptor->second.space,
            source_descriptor->second.position);
        if (!actor_cell)
        {
            instance_edit_error_ = "Actor position is outside its Space";
            return lux::cxx::unexpected(instance_edit_error_);
        }
        auto destination = std::ranges::find_if(
            authoring_instance_clusters_,
            [&](const auto& entry)
            {
                return entry.second.page.space ==
                        source_descriptor->second.space &&
                    entry.second.page.cell == *actor_cell;
            });
        if (destination == authoring_instance_clusters_.end())
        {
            instance_edit_error_ =
                "No materialized Instance Page exists in the Actor Cell";
            return lux::cxx::unexpected(instance_edit_error_);
        }

        auto* const persistent_entities =
            scenePersistentEntities(runtime_.get());
        if (!persistent_entities)
        {
            instance_edit_error_ =
                "SceneRuntime persistent entity index is unavailable";
            return lux::cxx::unexpected(instance_edit_error_);
        }
        WorldActorEcsAdapter adapter{
            components_,
            *persistent_entities};
        auto document = adapter.capture(
            registry,
            entity,
            world_source_->world,
            "Actor-to-Instance source");
        if (!document)
        {
            instance_edit_error_ = document.error();
            return lux::cxx::unexpected(instance_edit_error_);
        }
        document->actor_class = source_descriptor->second.actor_class;
        document->space = source_descriptor->second.space;
        document->position = source_descriptor->second.position;
        document->data_layers = source_descriptor->second.data_layers;
        document->references = source_descriptor->second.references;
        std::vector<std::string_view> allowed;
        for (const auto type : {
                 lux::ecs::typeToken<lux::ecs::NameComponent>(),
                 lux::ecs::typeToken<lux::ecs::Transform3DComponent>(),
                 lux::ecs::typeToken<lux::ecs::MeshComponent>()})
        {
            if (const auto* schema = components_.findByType(type))
                allowed.push_back(schema->fullName());
        }

        lux::authoring::EditableWorldInstance instance;
        instance.position = source_descriptor->second.position;
        instance.rotation = {
            transform->rotation.x(),
            transform->rotation.y(),
            transform->rotation.z(),
            transform->rotation.w()};
        instance.scale = {
            transform->scale.x(),
            transform->scale.y(),
            transform->scale.z()};
        instance.mesh = mesh->mesh_asset_id;
        instance.material_instance = mesh->material_asset_id;
        auto transaction = lux::authoring::convertActorToInstance(
            *world_source_,
            *descriptor_page,
            *document,
            destination->second.page,
            std::move(instance),
            allowed);
        if (!transaction)
        {
            instance_edit_error_ = transaction.error().detail;
            return lux::cxx::unexpected(instance_edit_error_);
        }
        auto validation_root = *world_source_;
        const auto validation_allocator = std::ranges::find(
            validation_root.instance_sets,
            transaction->instance_set.id,
            &lux::authoring::WorldInstanceSetSourceDescriptor::id);
        if (validation_allocator == validation_root.instance_sets.end())
            std::abort();
        validation_allocator->next_local_id =
            transaction->instance_set.next_local_id;
        if (!lux::authoring::encodeWorldInstancePage(
                validation_root, transaction->instance_page) ||
            !lux::authoring::encodeWorldDescriptorPage(
                validation_root, transaction->actor_descriptor_page))
        {
            instance_edit_error_ =
                "Actor-to-Instance transaction failed document validation";
            return lux::cxx::unexpected(instance_edit_error_);
        }
        const auto destination_before = destination->second;
        if (!activateAuthoringInstancePage(
                destination_before.descriptor_page,
                destination_before.descriptor,
                transaction->instance_page))
        {
            instance_edit_error_ =
                "Authoring Instance Page rejected Actor-to-Instance conversion";
            return lux::cxx::unexpected(instance_edit_error_);
        }

        const auto allocator = std::ranges::find(
            world_source_->instance_sets,
            transaction->instance_set.id,
            &lux::authoring::WorldInstanceSetSourceDescriptor::id);
        if (allocator == world_source_->instance_sets.end())
            std::abort();
        allocator->next_local_id = transaction->instance_set.next_local_id;

        cacheAuthoringDescriptorPage(
            transaction->actor_descriptor_page);
        dirty_instance_pages_.insert(destination->first);
        dirty_actor_ids_.erase(actor_key);
        materialized_actor_ids_.erase(actor_key);
        materialized_actor_descriptors_.erase(actor_key);
        selection_.releaseProxy(entity);
        registry.destroy(entity);
        authoring_load_result_.created_entities.erase(
            authoring_load_result_.created_entities.begin() +
                static_cast<std::ptrdiff_t>(actor_index));
        authoring_load_result_.world_entity_ids.erase(
            authoring_load_result_.world_entity_ids.begin() +
                static_cast<std::ptrdiff_t>(actor_index));
        selection_.selectObject(EditorObjectId{transaction->instance.id});
        trimAuthoringDescriptorPageCache();
        return transaction->instance.id;
    }

    lux::authoring::WorldDescriptorPageDocument*
    EditorScene::cachedAuthoringDescriptorPage(uuids::uuid page) noexcept
    {
        const auto found = world_descriptor_pages_.find(
            uuids::to_string(page));
        if (found == world_descriptor_pages_.end())
            return nullptr;
        found->second.last_use = ++descriptor_page_cache_clock_;
        return &found->second.document;
    }

    void EditorScene::cacheAuthoringDescriptorPage(lux::authoring::WorldDescriptorPageDocument page)
    {
        const auto key = uuids::to_string(page.id);
        const auto bytes = descriptorPageResidentBytes(page) +
            sizeof(CachedWorldDescriptorPage) +
            sizeof(std::string) + key.capacity();
        const auto found = world_descriptor_pages_.find(key);
        if (found != world_descriptor_pages_.end())
        {
            descriptor_page_resident_bytes_ -=
                found->second.resident_bytes;
            found->second = CachedWorldDescriptorPage{
                std::move(page),
                bytes,
                ++descriptor_page_cache_clock_};
        }
        else
        {
            world_descriptor_pages_.emplace(
                key,
                CachedWorldDescriptorPage{
                    std::move(page),
                    bytes,
                    ++descriptor_page_cache_clock_});
        }
        descriptor_page_resident_bytes_ += bytes;
    }

    void EditorScene::trimAuthoringDescriptorPageCache() noexcept
    {
        constexpr std::size_t kDescriptorPageCacheBudget =
            64u * 1024u * 1024u;
        while (descriptor_page_resident_bytes_ >
                   kDescriptorPageCacheBudget &&
               !world_descriptor_pages_.empty())
        {
            auto oldest = world_descriptor_pages_.end();
            for (auto candidate = world_descriptor_pages_.begin();
                 candidate != world_descriptor_pages_.end(); ++candidate)
            {
                const bool pinned = std::ranges::any_of(
                    authoring_instance_clusters_,
                    [&](const auto& cluster)
                    {
                        return dirty_instance_pages_.contains(cluster.first) &&
                            uuids::to_string(
                                cluster.second.descriptor_page) ==
                                candidate->first;
                    });
                if (pinned)
                    continue;
                if (oldest == world_descriptor_pages_.end() ||
                    candidate->second.last_use < oldest->second.last_use)
                {
                    oldest = candidate;
                }
            }
            if (oldest == world_descriptor_pages_.end())
                break;
            descriptor_page_resident_bytes_ -=
                oldest->second.resident_bytes;
            world_descriptor_pages_.erase(oldest);
        }
    }

    bool EditorScene::requestWorldActorProxy(
        lux::entity_scene::PersistentEntityId actor,
        bool select)
    {
        if (!live_ || !world_source_ || !world_descriptor_index_ ||
            actor.empty())
        {
            return false;
        }
        if (select)
            selection_.selectObject(EditorObjectId{actor});
        const auto key = uuids::to_string(actor.value());
        if (materialized_actor_ids_.contains(key))
        {
            for (std::size_t index = 0u;
                 index < authoring_load_result_.world_entity_ids.size();
                 ++index)
            {
                if (authoring_load_result_.world_entity_ids[index] == actor)
                {
                    const auto entity =
                        authoring_load_result_.created_entities[index];
                    if (runtime_->world().registry().valid(entity))
                    {
                        selection_.resolveProxy(
                            &runtime_->world().registry(),
                            entity,
                            EditorObjectId{actor});
                    }
                    break;
                }
            }
            return true;
        }
        if (!pending_actor_proxies_.insert(key).second)
        {
            return true;
        }
        const auto persistent_actor =
            lux::entity_scene::PersistentEntityId{actor.value()};
        const auto indexed = world_descriptor_index_->find(persistent_actor);
        const auto* page = indexed
            ? world_descriptor_index_->page(indexed->descriptor_page)
            : nullptr;
        if (!indexed || !page)
        {
            pending_actor_proxies_.erase(key);
            return false;
        }
        std::optional<lux::authoring::WorldActorSourceDescriptor>
            cached_descriptor;
        const auto* loaded_page = cachedAuthoringDescriptorPage(
            indexed->descriptor_page);
        if (loaded_page)
        {
            const auto descriptor = std::ranges::find(
                loaded_page->actors,
                persistent_actor,
                &lux::authoring::WorldActorSourceDescriptor::id);
            if (descriptor == loaded_page->actors.end())
            {
                pending_actor_proxies_.erase(key);
                return false;
            }
            cached_descriptor = *descriptor;
        }
        const auto page_key = uuids::to_string(indexed->descriptor_page);
        const bool loads_descriptor_page = !cached_descriptor.has_value();
        if (loads_descriptor_page &&
            !pending_descriptor_pages_.insert(page_key).second)
        {
            pending_actor_proxies_.erase(key);
            return true;
        }
        const auto world_id = world_source_->world;
        const std::weak_ptr<PlayCookControl> weak = play_cook_control_;
        auto source = std::make_shared<const
            lux::authoring::WorldSourceDocument>(*world_source_);
        const bool admitted = editor_async_.loadWorldActorProxy(
            LoadWorldActorProxyOperation{
                scene_path_,
                std::move(source),
                *page,
                actor,
                std::move(cached_descriptor)},
            [weak, world_id, key, page_key, loads_descriptor_page](
                auto outcome) mutable
            {
                const auto control = weak.lock();
                if (!control || !control->owner)
                    return;
                auto* owner = control->owner;
                owner->pending_actor_proxies_.erase(key);
                if (loads_descriptor_page)
                    owner->pending_descriptor_pages_.erase(page_key);
                if (!owner->world_source_ ||
                    owner->world_source_->world != world_id || !outcome ||
                    !outcome->document)
                {
                    return;
                }
                auto& registry = owner->runtime_->world().registry();
                auto* const persistent_entities =
                    scenePersistentEntities(owner->runtime_.get());
                if (!persistent_entities)
                {
                    lux::log::error(
                        "editor",
                        "Authoring Actor proxy materialization has no "
                        "SceneRuntime persistent entity index");
                    return;
                }
                WorldActorEcsAdapter adapter{
                    owner->components_,
                    *persistent_entities};
                auto materialized = adapter.materialize(
                    *outcome->document,
                    registry,
                    outcome->descriptor.display_name);
                if (!materialized)
                {
                    lux::log::error(
                        "editor",
                        "Authoring Actor proxy materialization failed: {}",
                        materialized.error());
                    return;
                }
                const auto entity = *materialized;
                if (outcome->page)
                    owner->cacheAuthoringDescriptorPage(
                        std::move(*outcome->page));
                owner->authoring_load_result_.created_entities.push_back(
                    entity);
                owner->authoring_load_result_.world_entity_ids.push_back(
                    lux::entity_scene::PersistentEntityId{
                        outcome->descriptor.id.value()});
                owner->materialized_actor_ids_.insert(key);
                owner->materialized_actor_descriptors_.insert_or_assign(
                    key,
                    outcome->descriptor);
                if (registry.any_of<lux::ecs::PrimaryCameraTag>(entity))
                {
                    owner->authoring_load_result_.primary_camera = entity;
                    owner->restoreAuthoringViewpoint(entity);
                }
                owner->selection_.resolveProxy(
                    &registry,
                    entity,
                    EditorObjectId{lux::entity_scene::PersistentEntityId{
                        outcome->descriptor.id.value()}});
                owner->updateAuthoringProxyWindow();
            });
        if (!admitted)
        {
            pending_actor_proxies_.erase(key);
            if (loads_descriptor_page)
                pending_descriptor_pages_.erase(page_key);
        }
        return admitted;
    }
