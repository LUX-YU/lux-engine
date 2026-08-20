    bool EditorScene::requestWorldTerrainRegion(
        lux::authoring::TerrainSetId terrain,
        lux::authoring::PartitionSpaceId space,
        std::vector<lux::authoring::WorldCellKey> cells)
    {
        terrain_edit_error_.clear();
        if (!live_ || !world_source_ || scene_path_.empty() ||
            terrain.empty() || space.empty() || cells.empty())
        {
            terrain_edit_error_ = "Terrain region request is incomplete";
            return false;
        }
        std::unordered_set<std::string> requested;
        std::erase_if(
            cells,
            [&](const auto& cell)
            {
                return !requested.insert(terrainPageKey(terrain, cell)).second;
            });
        const auto all_loaded = std::ranges::all_of(
            cells,
            [&](const auto& cell)
            {
                return terrain_pages_.contains(
                    terrainPageKey(terrain, cell));
            });
        if (all_loaded)
        {
            selection_.selectObject(EditorObjectId{TerrainSelection{
                terrain.value(), cells.front(), 0u}});
            return true;
        }
        if (terrain_load_pending_)
        {
            terrain_edit_error_ = "another Terrain region load is pending";
            return false;
        }

        terrain_load_pending_ = true;
        const auto world_id = world_source_->world;
        const auto selected_cell = cells.front();
        const std::weak_ptr<PlayCookControl> weak = play_cook_control_;
        auto source = std::make_shared<const
            lux::authoring::WorldSourceDocument>(*world_source_);
        const bool admitted = editor_async_.loadWorldTerrainRegion(
            LoadWorldTerrainRegionOperation{
                scene_path_,
                std::move(source),
                terrain,
                space,
                std::move(cells)},
            [weak, world_id, terrain, selected_cell](auto outcome) mutable
            {
                const auto control = weak.lock();
                if (!control || !control->owner)
                    return;
                auto* owner = control->owner;
                owner->terrain_load_pending_ = false;
                if (!owner->world_source_ ||
                    owner->world_source_->world != world_id)
                {
                    return;
                }
                if (!outcome || !outcome->error.empty())
                {
                    owner->terrain_edit_error_ = outcome
                        ? std::move(outcome->error)
                        : "Terrain region operation failed";
                    return;
                }
                for (auto& descriptor_page : outcome->descriptor_pages)
                    owner->cacheAuthoringDescriptorPage(
                        std::move(descriptor_page));
                owner->trimAuthoringDescriptorPageCache();
                for (auto& page : outcome->pages)
                {
                    const auto key = terrainPageKey(
                        page.terrain_set, page.cell);
                    if (!owner->dirty_terrain_pages_.contains(key))
                        owner->terrain_pages_.insert_or_assign(
                            key, std::move(page));
                }
                owner->terrain_edit_error_.clear();
                owner->selection_.selectObject(EditorObjectId{
                    TerrainSelection{
                        terrain.value(), selected_cell, 0u}});
            });
        if (!admitted)
        {
            terrain_load_pending_ = false;
            terrain_edit_error_ = "Terrain region load was not admitted";
        }
        return admitted;
    }

    bool EditorScene::applyWorldTerrainBrush(
        lux::authoring::TerrainSetId terrain,
        std::span<const lux::authoring::WorldCellKey> cells,
        const lux::spatial::Position3D& center,
        const lux::authoring::WorldTerrainBrush& brush)
    {
        terrain_edit_error_.clear();
        if (!world_source_ || terrain.empty() || cells.empty())
        {
            terrain_edit_error_ = "Terrain brush has no loaded Page region";
            return false;
        }
        std::vector<lux::authoring::WorldTerrainPageDocument> pages;
        pages.reserve(cells.size());
        std::unordered_set<std::string> unique;
        for (const auto& cell : cells)
        {
            const auto key = terrainPageKey(terrain, cell);
            if (!unique.insert(key).second)
                continue;
            const auto found = terrain_pages_.find(key);
            if (found == terrain_pages_.end())
            {
                terrain_edit_error_ =
                    "Terrain brush references an unloaded Page";
                return false;
            }
            pages.push_back(found->second);
        }
        auto transaction = lux::authoring::applyWorldTerrainBrush(
            *world_source_, pages, center, brush);
        if (!transaction)
        {
            terrain_edit_error_ = transaction.error().detail;
            return false;
        }
        if (transaction->after_pages.empty())
            return true;
        for (const auto& page : transaction->after_pages)
        {
            const auto key = terrainPageKey(page.terrain_set, page.cell);
            terrain_pages_.insert_or_assign(key, page);
            dirty_terrain_pages_.insert(key);
        }
        terrain_undo_stack_.push_back(std::move(*transaction));
        terrain_redo_stack_.clear();
        selection_.selectObject(EditorObjectId{TerrainSelection{
            terrain.value(), cells.front(), 0u}});
        return true;
    }

    bool EditorScene::undoWorldTerrainEdit()
    {
        if (terrain_undo_stack_.empty())
            return false;
        auto transaction = std::move(terrain_undo_stack_.back());
        terrain_undo_stack_.pop_back();
        for (const auto& page : transaction.before_pages)
        {
            const auto key = terrainPageKey(page.terrain_set, page.cell);
            terrain_pages_.insert_or_assign(key, page);
            dirty_terrain_pages_.insert(key);
        }
        terrain_redo_stack_.push_back(std::move(transaction));
        terrain_edit_error_.clear();
        return true;
    }

    bool EditorScene::redoWorldTerrainEdit()
    {
        if (terrain_redo_stack_.empty())
            return false;
        auto transaction = std::move(terrain_redo_stack_.back());
        terrain_redo_stack_.pop_back();
        for (const auto& page : transaction.after_pages)
        {
            const auto key = terrainPageKey(page.terrain_set, page.cell);
            terrain_pages_.insert_or_assign(key, page);
            dirty_terrain_pages_.insert(key);
        }
        terrain_undo_stack_.push_back(std::move(transaction));
        terrain_edit_error_.clear();
        return true;
    }

    lux::cxx::expected<
        lux::authoring::WorldTerrainHeightmap16,
        lux::authoring::WorldTerrainAuthoringFailure>
    EditorScene::exportWorldTerrainHeightmap16(
        lux::authoring::TerrainSetId terrain,
        std::span<const lux::authoring::WorldCellKey> cells) const
    {
        std::vector<lux::authoring::WorldTerrainPageDocument> pages;
        pages.reserve(cells.size());
        for (const auto& cell : cells)
        {
            const auto found = terrain_pages_.find(
                terrainPageKey(terrain, cell));
            if (found == terrain_pages_.end())
            {
                return lux::cxx::unexpected(
                    lux::authoring::WorldTerrainAuthoringFailure{
                        lux::authoring::EWorldTerrainAuthoringError::
                            INVALID_ARGUMENT,
                        "heightmap export references an unloaded Page"});
            }
            pages.push_back(found->second);
        }
        if (!world_source_)
        {
            return lux::cxx::unexpected(
                lux::authoring::WorldTerrainAuthoringFailure{
                    lux::authoring::EWorldTerrainAuthoringError::
                        INVALID_ARGUMENT,
                    "heightmap export has no Authoring World"});
        }
        return lux::authoring::exportWorldTerrainHeightmap16(
            *world_source_, pages);
    }

    bool EditorScene::importWorldTerrainHeightmap16(
        lux::authoring::TerrainSetId terrain,
        std::span<const lux::authoring::WorldCellKey> cells,
        const lux::authoring::WorldTerrainHeightmap16& image)
    {
        terrain_edit_error_.clear();
        if (!world_source_)
            return false;
        std::vector<lux::authoring::WorldTerrainPageDocument> pages;
        pages.reserve(cells.size());
        for (const auto& cell : cells)
        {
            const auto found = terrain_pages_.find(
                terrainPageKey(terrain, cell));
            if (found == terrain_pages_.end())
            {
                terrain_edit_error_ =
                    "heightmap import references an unloaded Page";
                return false;
            }
            pages.push_back(found->second);
        }
        auto transaction = lux::authoring::importWorldTerrainHeightmap16(
            *world_source_, pages, image);
        if (!transaction)
        {
            terrain_edit_error_ = transaction.error().detail;
            return false;
        }
        for (const auto& page : transaction->after_pages)
        {
            const auto key = terrainPageKey(page.terrain_set, page.cell);
            terrain_pages_.insert_or_assign(key, page);
            dirty_terrain_pages_.insert(key);
        }
        terrain_undo_stack_.push_back(std::move(*transaction));
        terrain_redo_stack_.clear();
        return true;
    }

    bool EditorScene::requestImportWorldTerrainHeightmap16(
        lux::authoring::TerrainSetId terrain,
        std::vector<lux::authoring::WorldCellKey> cells,
        std::filesystem::path raw16_file)
    {
        terrain_edit_error_.clear();
        if (terrain_heightmap_io_pending_ || raw16_file.empty())
        {
            terrain_edit_error_ = terrain_heightmap_io_pending_
                ? "another Terrain heightmap IO request is pending"
                : "RAW16 import path is empty";
            return false;
        }
        auto image = exportWorldTerrainHeightmap16(terrain, cells);
        if (!image)
        {
            terrain_edit_error_ = image.error().detail;
            return false;
        }
        image->samples.clear();
        terrain_heightmap_io_pending_ = true;
        const auto world_id = world_source_->world;
        const std::weak_ptr<PlayCookControl> weak = play_cook_control_;
        const bool admitted = editor_async_.worldTerrainHeightmapFile(
            WorldTerrainHeightmapFileOperation{
                EWorldTerrainHeightmapFileMode::READ,
                std::move(raw16_file),
                std::move(*image)},
            [weak, world_id, terrain, cells = std::move(cells)](
                auto outcome) mutable
            {
                const auto control = weak.lock();
                if (!control || !control->owner)
                    return;
                auto* owner = control->owner;
                owner->terrain_heightmap_io_pending_ = false;
                if (!owner->world_source_ ||
                    owner->world_source_->world != world_id)
                {
                    return;
                }
                if (!outcome || !outcome->error.empty())
                {
                    owner->terrain_edit_error_ = outcome
                        ? std::move(outcome->error)
                        : "RAW16 import operation failed";
                    return;
                }
                (void)owner->importWorldTerrainHeightmap16(
                    terrain, cells, outcome->image);
            });
        if (!admitted)
        {
            terrain_heightmap_io_pending_ = false;
            terrain_edit_error_ = "RAW16 import was not admitted";
        }
        return admitted;
    }

    bool EditorScene::requestExportWorldTerrainHeightmap16(
        lux::authoring::TerrainSetId terrain,
        std::vector<lux::authoring::WorldCellKey> cells,
        std::filesystem::path raw16_file)
    {
        terrain_edit_error_.clear();
        if (terrain_heightmap_io_pending_ || raw16_file.empty())
        {
            terrain_edit_error_ = terrain_heightmap_io_pending_
                ? "another Terrain heightmap IO request is pending"
                : "RAW16 export path is empty";
            return false;
        }
        auto image = exportWorldTerrainHeightmap16(terrain, cells);
        if (!image)
        {
            terrain_edit_error_ = image.error().detail;
            return false;
        }
        terrain_heightmap_io_pending_ = true;
        const auto world_id = world_source_->world;
        const std::weak_ptr<PlayCookControl> weak = play_cook_control_;
        const bool admitted = editor_async_.worldTerrainHeightmapFile(
            WorldTerrainHeightmapFileOperation{
                EWorldTerrainHeightmapFileMode::WRITE,
                std::move(raw16_file),
                std::move(*image)},
            [weak, world_id](auto outcome) mutable
            {
                const auto control = weak.lock();
                if (!control || !control->owner)
                    return;
                auto* owner = control->owner;
                owner->terrain_heightmap_io_pending_ = false;
                if (!owner->world_source_ ||
                    owner->world_source_->world != world_id)
                {
                    return;
                }
                owner->terrain_edit_error_ = !outcome
                    ? "RAW16 export operation failed"
                    : std::move(outcome->error);
            });
        if (!admitted)
        {
            terrain_heightmap_io_pending_ = false;
            terrain_edit_error_ = "RAW16 export was not admitted";
        }
        return admitted;
    }

    WorldTerrainEditStats EditorScene::worldTerrainEditStats() const
    {
        return {
            terrain_pages_.size(),
            dirty_terrain_pages_.size(),
            terrain_undo_stack_.size(),
            terrain_redo_stack_.size(),
            terrain_load_pending_,
            terrain_heightmap_io_pending_,
            terrain_edit_error_};
    }

    std::optional<lux::authoring::PartitionSpaceId>
    EditorScene::defaultWorldTerrainSpace() const
    {
        if (!world_source_)
            return std::nullopt;
        const auto found = std::ranges::find_if(
            world_source_->spaces,
            [](const auto& space)
            {
                return space.topology ==
                    lux::authoring::EPartitionTopology::PLANAR_XZ;
            });
        return found == world_source_->spaces.end()
            ? std::nullopt
            : std::optional{found->id};
    }

    std::optional<lux::spatial::Position3D>
    EditorScene::makeWorldTerrainPosition(
        lux::authoring::PartitionSpaceId space_id,
        const lux::authoring::WorldCellKey& cell,
        float local_x,
        float local_z) const
    {
        if (!world_source_ || !std::isfinite(local_x) ||
            !std::isfinite(local_z) ||
            cell.topology != lux::authoring::EPartitionTopology::PLANAR_XZ)
        {
            return std::nullopt;
        }
        const auto space = std::ranges::find(
            world_source_->spaces,
            space_id,
            &lux::authoring::PartitionSpaceDescriptor::id);
        const auto* coordinate = std::get_if<
            lux::authoring::PlanarCellCoord>(&cell.coordinate);
        if (space == world_source_->spaces.end() || !coordinate ||
            local_x < 0.0f || local_z < 0.0f ||
            local_x > space->cell_edge || local_z > space->cell_edge)
        {
            return std::nullopt;
        }
        const auto x = static_cast<long double>(coordinate->a) *
                space->cell_edge + local_x;
        const auto z = static_cast<long double>(coordinate->b) *
                space->cell_edge + local_z;
        if (!std::isfinite(static_cast<double>(x)) ||
            !std::isfinite(static_cast<double>(z)))
        {
            return std::nullopt;
        }
        return lux::spatial::Position3D{
            static_cast<double>(x),
            0.0,
            static_cast<double>(z)};
    }

    void EditorScene::requestAuthoringDescriptorPage(const lux::authoring::WorldDescriptorPageReference& page)
    {
        if (!live_ || !world_source_ || scene_path_.empty() ||
            page.id.is_nil())
        {
            return;
        }
        const auto key = uuids::to_string(page.id);
        if (pending_viewport_descriptor_pages_.contains(key) ||
            world_descriptor_pages_.contains(key))
        {
            return;
        }
        pending_viewport_descriptor_pages_.insert(key);
        const auto world_id = world_source_->world;
        const std::weak_ptr<PlayCookControl> weak = play_cook_control_;
        auto source = std::make_shared<const
            lux::authoring::WorldSourceDocument>(*world_source_);
        const bool admitted = editor_async_.loadWorldDescriptorPage(
            LoadWorldDescriptorPageOperation{
                scene_path_, std::move(source), page},
            [weak, world_id, key](auto outcome) mutable
            {
                const auto control = weak.lock();
                if (!control || !control->owner)
                    return;
                auto* owner = control->owner;
                owner->pending_viewport_descriptor_pages_.erase(key);
                if (!owner->live_ || !owner->world_source_ ||
                    owner->world_source_->world != world_id)
                {
                    return;
                }
                if (!outcome || !outcome->page)
                {
                    lux::log::error(
                        "editor",
                        "Authoring Descriptor Page load failed: {}",
                        outcome ? outcome->error :
                            std::string{"operation failed"});
                    return;
                }
                owner->cacheAuthoringDescriptorPage(
                    std::move(*outcome->page));
                owner->updateAuthoringProxyWindow();
            });
        if (!admitted)
            pending_viewport_descriptor_pages_.erase(key);
    }

    void EditorScene::requestAuthoringInstancePage(
        uuids::uuid descriptor_page,
        const lux::authoring::WorldPageSourceDescriptor& page)
    {
        if (!live_ || !world_source_ || scene_path_.empty() ||
            page.id.is_nil() || page.kind !=
                lux::authoring::EWorldPageSourceKind::INSTANCE)
        {
            return;
        }
        const auto key = uuids::to_string(page.id);
        if (pending_instance_pages_.contains(key) ||
            authoring_instance_clusters_.contains(key))
        {
            return;
        }
        pending_instance_pages_.insert(key);
        const auto world_id = world_source_->world;
        const std::weak_ptr<PlayCookControl> weak = play_cook_control_;
        auto source = std::make_shared<const
            lux::authoring::WorldSourceDocument>(*world_source_);
        const bool admitted = editor_async_.loadWorldInstancePage(
            LoadWorldInstancePageOperation{
                scene_path_, std::move(source), page},
            [weak, world_id, key, descriptor_page](auto outcome) mutable
            {
                const auto control = weak.lock();
                if (!control || !control->owner)
                    return;
                auto* owner = control->owner;
                owner->pending_instance_pages_.erase(key);
                if (!owner->live_ || !owner->world_source_ ||
                    owner->world_source_->world != world_id ||
                    !owner->desired_instance_pages_.contains(key))
                {
                    return;
                }
                if (!outcome || !outcome->page ||
                    !owner->activateAuthoringInstancePage(
                        descriptor_page,
                        std::move(outcome->descriptor),
                        std::move(*outcome->page)))
                {
                    lux::log::error(
                        "editor",
                        "Authoring Instance Page activation failed: {}",
                        outcome ? outcome->error :
                            std::string{"operation failed"});
                }
            });
        if (!admitted)
            pending_instance_pages_.erase(key);
    }

    bool EditorScene::activateAuthoringInstancePage(
        uuids::uuid descriptor_page,
        lux::authoring::WorldPageSourceDescriptor descriptor,
        lux::authoring::WorldInstancePageDocument page)
    {
        constexpr std::size_t kMaximumEditorInstancesPerPage = 65'536u;
        constexpr std::size_t kMaximumEditorResidentInstances = 1'000'000u;
        if (!runtime_ || !world_source_ || page.world != world_source_->world ||
            descriptor.kind !=
                lux::authoring::EWorldPageSourceKind::INSTANCE ||
            descriptor.id.is_nil() || page.instances.size() >
                kMaximumEditorInstancesPerPage)
        {
            return false;
        }
        const auto key = uuids::to_string(descriptor.id);
        const auto existing = authoring_instance_clusters_.find(key);
        const auto previous_count = existing ==
                authoring_instance_clusters_.end()
            ? 0u
            : existing->second.page.instances.size();
        if (page.instances.size() > previous_count &&
            page.instances.size() - previous_count >
                kMaximumEditorResidentInstances -
                    std::min(
                        authoring_instance_resident_count_,
                        kMaximumEditorResidentInstances))
        {
            return false;
        }

        authoring_instance_clusters_.insert_or_assign(
            key,
            AuthoringInstanceCluster{
                descriptor_page,
                std::move(descriptor),
                std::move(page)});
        authoring_instance_resident_count_ =
            authoring_instance_resident_count_ - previous_count +
            authoring_instance_clusters_.at(key).page.instances.size();
        // Authoring has no safe way to publish an arbitrary mesh payload into
        // EntityScene's generic BlobStore yet. Keep the editable LXIP data,
        // but make the missing preview explicit instead of rebuilding the old
        // parallel render registry.
        instance_preview_status_ =
            "Instance data is editable, but its viewport preview is "
            "unavailable until the EntityScene BlobStore authoring seam is "
            "installed.";
        return true;
    }

    void EditorScene::clearAuthoringInstanceClusters() noexcept
    {
        authoring_instance_clusters_.clear();
        authoring_instance_resident_count_ = 0u;
        desired_instance_pages_.clear();
        dirty_instance_pages_.clear();
        instance_edit_error_.clear();
        instance_preview_status_.clear();
        pending_instance_pages_.clear();
        pending_viewport_descriptor_pages_.clear();
    }

    void EditorScene::updateAuthoringProxyWindow()
    {
        if (!world_source_ || !world_descriptor_index_ ||
            camera_entity_ == entt::null)
        {
            return;
        }
        auto& registry = runtime_->world().registry();
        if (!registry.valid(camera_entity_))
            return;

        lux::authoring::WorldActorSourcePosition camera_position;
        if (const auto* transform = registry.try_get<
                lux::ecs::ResolvedTransform2DComponent>(camera_entity_))
        {
            camera_position = transform->position;
        }
        else if (const auto* transform = registry.try_get<
                     lux::ecs::ResolvedTransform3DComponent>(camera_entity_))
        {
            camera_position = transform->position;
        }
        else
            return;

        const bool is_2d = std::holds_alternative<
            lux::spatial::Position2D>(camera_position);
        const auto space = std::ranges::find_if(
            world_source_->spaces,
            [is_2d](const auto& candidate)
            {
                return is_2d
                    ? candidate.topology ==
                          lux::authoring::EPartitionTopology::PLANAR_XY
                    : candidate.topology !=
                          lux::authoring::EPartitionTopology::PLANAR_XY;
            });
        if (space == world_source_->spaces.end())
            return;
        const auto center_macro = descriptorMacro(
            *world_source_, space->id, camera_position);
        if (!center_macro)
            return;

        const auto camera_cell = descriptorCell(
            *world_source_, space->id, camera_position);
        if (!camera_cell)
            return;

        float proxy_radius = 512.0f;
        const auto settings = registry.view<lux::ecs::SceneSettingsComponent>();
        if (!settings.empty())
        {
            const auto candidate = settings.get<
                lux::ecs::SceneSettingsComponent>(*settings.begin()).cull_distance;
            if (std::isfinite(candidate) && candidate > 0.0f)
                proxy_radius = candidate;
        }
        const auto cells = static_cast<std::int64_t>(std::ceil(
            static_cast<double>(proxy_radius) /
            static_cast<double>(space->cell_edge)));
        const auto macro_radius_unbounded =
            cells / static_cast<std::int64_t>(space->macro_edge_cells) + 1;
        const auto macro_radius = std::min<std::int64_t>(
            macro_radius_unbounded,
            space->topology ==
                    lux::authoring::EPartitionTopology::VOLUMETRIC_XYZ
                ? 7
                : 31);

        std::unordered_set<std::string> desired;
        desired_instance_pages_.clear();
        std::size_t proxy_admissions = 0u;
        std::size_t instance_page_admissions = 0u;
        constexpr std::size_t kMaximumProxyAdmissionsPerUpdate = 256u;
        constexpr std::size_t kMaximumMaterializedActorProxies = 10'000u;
        constexpr std::size_t kMaximumInstancePageAdmissionsPerUpdate = 16u;
        constexpr std::size_t kMaximumResidentInstancePages = 256u;
        constexpr std::size_t kMaximumPendingDescriptorPages = 64u;
        const auto axisWithin = [](std::int64_t value,
                                   std::int64_t center,
                                   std::int64_t radius)
        {
            const auto lower = center <
                    std::numeric_limits<std::int64_t>::min() + radius
                ? std::numeric_limits<std::int64_t>::min()
                : center - radius;
            const auto upper = center >
                    std::numeric_limits<std::int64_t>::max() - radius
                ? std::numeric_limits<std::int64_t>::max()
                : center + radius;
            return value >= lower && value <= upper;
        };
        const auto cellInWindow = [&](const lux::authoring::WorldCellKey& cell)
        {
            if (cell.topology != camera_cell->topology)
                return false;
            if (const auto* point = std::get_if<
                    lux::authoring::PlanarCellCoord>(&cell.coordinate))
            {
                const auto* center = std::get_if<
                    lux::authoring::PlanarCellCoord>(&camera_cell->coordinate);
                return center && axisWithin(point->a, center->a, cells + 1) &&
                    axisWithin(point->b, center->b, cells + 1);
            }
            const auto* point = std::get_if<
                lux::authoring::VolumeCellCoord>(&cell.coordinate);
            const auto* center = std::get_if<
                lux::authoring::VolumeCellCoord>(&camera_cell->coordinate);
            return point && center &&
                axisWithin(point->x, center->x, cells + 1) &&
                axisWithin(point->y, center->y, cells + 1) &&
                axisWithin(point->z, center->z, cells + 1);
        };
        const auto visitMacro = [&](const lux::authoring::WorldMacroCoord& macro)
        {
            const auto* page = world_descriptor_index_->page(space->id, macro);
            if (!page)
                return;
            const auto* loaded_page = cachedAuthoringDescriptorPage(
                page->id);
            if (!loaded_page)
            {
                if (pending_viewport_descriptor_pages_.size() <
                    kMaximumPendingDescriptorPages)
                {
                    requestAuthoringDescriptorPage(*page);
                }
            }
            else
            {
                for (const auto& source_page : loaded_page->pages)
                {
                    if (source_page.kind !=
                            lux::authoring::EWorldPageSourceKind::INSTANCE ||
                        !cellInWindow(source_page.cell))
                    {
                        continue;
                    }
                    const auto key = uuids::to_string(source_page.id);
                    desired_instance_pages_.insert(key);
                    if (instance_page_admissions <
                            kMaximumInstancePageAdmissionsPerUpdate &&
                        !authoring_instance_clusters_.contains(key) &&
                        !pending_instance_pages_.contains(key) &&
                        authoring_instance_clusters_.size() +
                                pending_instance_pages_.size() <
                            kMaximumResidentInstancePages)
                    {
                        ++instance_page_admissions;
                        requestAuthoringInstancePage(
                            loaded_page->id,
                            source_page);
                    }
                }
            }
            const auto actors = world_descriptor_index_->actorsInPage(page->id);
            for (const auto actor : actors)
            {
                const auto indexed = world_descriptor_index_->find(actor);
                if (!indexed)
                    continue;
                const auto maximum_bound = std::max({
                    indexed->bounds_half_extent[0],
                    indexed->bounds_half_extent[1],
                    indexed->bounds_half_extent[2]});
                const auto maximum_extent = proxy_radius + maximum_bound + 1.0f;
                bool intersects = false;
                if (const auto* camera_2d = std::get_if<
                        lux::spatial::Position2D>(&camera_position))
                {
                    const auto* actor_2d = std::get_if<
                        lux::spatial::Position2D>(&indexed->position);
                    const auto delta = actor_2d
                        ? lux::spatial::relativeFloat(
                              *actor_2d,
                              *camera_2d,
                              maximum_extent)
                        : std::nullopt;
                    intersects = delta &&
                        std::abs((*delta)[0]) <=
                            proxy_radius + indexed->bounds_half_extent[0] &&
                        std::abs((*delta)[1]) <=
                            proxy_radius + indexed->bounds_half_extent[1];
                }
                else
                {
                    const auto* camera_3d = std::get_if<
                        lux::spatial::Position3D>(&camera_position);
                    const auto* actor_3d = std::get_if<
                        lux::spatial::Position3D>(&indexed->position);
                    const auto delta = camera_3d && actor_3d
                        ? lux::spatial::relativeFloat(
                              *actor_3d,
                              *camera_3d,
                              maximum_extent)
                        : std::nullopt;
                    intersects = delta &&
                        std::abs((*delta)[0]) <=
                            proxy_radius + indexed->bounds_half_extent[0] &&
                        std::abs((*delta)[2]) <=
                            proxy_radius + indexed->bounds_half_extent[2] &&
                        (space->topology ==
                                 lux::authoring::EPartitionTopology::PLANAR_XZ ||
                         std::abs((*delta)[1]) <=
                             proxy_radius + indexed->bounds_half_extent[1]);
                }
                if (!intersects)
                    continue;
                const auto key = uuids::to_string(actor.value());
                desired.insert(key);
                if (loaded_page)
                {
                    const auto descriptor = std::ranges::find(
                        loaded_page->actors,
                        actor,
                        &lux::authoring::WorldActorSourceDescriptor::id);
                    if (descriptor != loaded_page->actors.end())
                    {
                        for (const auto& reference : descriptor->references)
                        {
                            if (reference.kind != lux::authoring::
                                    EWorldActorReferenceKind::LOCAL)
                                continue;
                            const auto dependency_key = uuids::to_string(
                                reference.target.value());
                            desired.insert(dependency_key);
                            if (proxy_admissions <
                                    kMaximumProxyAdmissionsPerUpdate &&
                                !materialized_actor_ids_.contains(
                                    dependency_key) &&
                                !pending_actor_proxies_.contains(
                                    dependency_key))
                            {
                                ++proxy_admissions;
                                (void)requestWorldActorProxy(
                                    lux::authoring::WorldActorId{
                                        reference.target.value()},
                                    false);
                            }
                        }
                    }
                }
                if (proxy_admissions < kMaximumProxyAdmissionsPerUpdate &&
                    materialized_actor_ids_.size() +
                            pending_actor_proxies_.size() <
                        kMaximumMaterializedActorProxies &&
                    !materialized_actor_ids_.contains(key) &&
                    !pending_actor_proxies_.contains(key))
                {
                    ++proxy_admissions;
                    (void)requestWorldActorProxy(
                        lux::authoring::WorldActorId{actor.value()},
                        false);
                }
            }
        };
        if (const auto* planar = std::get_if<lux::authoring::PlanarMacroCoord>(
                &center_macro->coordinate))
        {
            for (std::int64_t radius = 0; radius <= macro_radius; ++radius)
            for (std::int64_t a = -radius; a <= radius; ++a)
            for (std::int64_t b = -radius; b <= radius; ++b)
            {
                if (std::max(std::abs(a), std::abs(b)) != radius)
                    continue;
                if ((a < 0 && planar->a <
                        std::numeric_limits<std::int64_t>::min() - a) ||
                    (a > 0 && planar->a >
                        std::numeric_limits<std::int64_t>::max() - a) ||
                    (b < 0 && planar->b <
                        std::numeric_limits<std::int64_t>::min() - b) ||
                    (b > 0 && planar->b >
                        std::numeric_limits<std::int64_t>::max() - b))
                {
                    continue;
                }
                visitMacro(lux::authoring::WorldMacroCoord{
                    space->topology,
                    lux::authoring::PlanarMacroCoord{
                        planar->a + a, planar->b + b}});
            }
        }
        else if (const auto* volume = std::get_if<
                     lux::authoring::VolumeMacroCoord>(&center_macro->coordinate))
        {
            for (std::int64_t radius = 0; radius <= macro_radius; ++radius)
            for (std::int64_t x = -radius; x <= radius; ++x)
            for (std::int64_t y = -radius; y <= radius; ++y)
            for (std::int64_t z = -radius; z <= radius; ++z)
            {
                if (std::max({std::abs(x), std::abs(y), std::abs(z)}) !=
                    radius)
                {
                    continue;
                }
                const auto safe = [](std::int64_t value, std::int64_t delta)
                {
                    return (delta >= 0 && value <=
                                std::numeric_limits<std::int64_t>::max() - delta) ||
                        (delta < 0 && value >=
                                std::numeric_limits<std::int64_t>::min() - delta);
                };
                if (!safe(volume->x, x) || !safe(volume->y, y) ||
                    !safe(volume->z, z))
                {
                    continue;
                }
                visitMacro(lux::authoring::WorldMacroCoord{
                    space->topology,
                    lux::authoring::VolumeMacroCoord{
                        volume->x + x,
                        volume->y + y,
                        volume->z + z}});
            }
        }
        if (const auto& selected = selection_.object(); selected)
        {
            if (const auto* actor = std::get_if<
                    lux::authoring::WorldActorId>(&*selected))
            {
                desired.insert(uuids::to_string(actor->value()));
            }
            else if (const auto* instance = std::get_if<
                         lux::authoring::WorldInstanceId>(&*selected))
            {
                for (const auto& [key, page] :
                     authoring_instance_clusters_)
                {
                    if (std::ranges::any_of(
                            page.page.instances,
                            [instance](const auto& candidate)
                            {
                                return candidate.id == *instance;
                            }))
                    {
                        desired_instance_pages_.insert(key);
                        break;
                    }
                }
            }
        }
        desired_instance_pages_.insert(
            dirty_instance_pages_.begin(),
            dirty_instance_pages_.end());
        desired.insert(dirty_actor_ids_.begin(), dirty_actor_ids_.end());

        std::vector<std::string> expired_pages;
        for (const auto& [key, page] : authoring_instance_clusters_)
        {
            if (!desired_instance_pages_.contains(key))
                expired_pages.push_back(key);
        }
        for (const auto& key : expired_pages)
        {
            const auto found = authoring_instance_clusters_.find(key);
            if (found != authoring_instance_clusters_.end())
            {
                authoring_instance_resident_count_ -=
                    found->second.page.instances.size();
                authoring_instance_clusters_.erase(found);
            }
        }

        std::vector<std::string> candidates;
        candidates.reserve(materialized_actor_ids_.size());
        for (const auto& actor : materialized_actor_ids_)
        {
            if (!desired.contains(actor))
                candidates.push_back(actor);
        }
        auto* const persistent_entities =
            scenePersistentEntities(runtime_.get());
        if (!persistent_entities)
        {
            lux::log::error(
                "editor",
                "Cannot evict Authoring Actor proxies without the "
                "SceneRuntime persistent entity index");
            return;
        }
        WorldActorEcsAdapter adapter{
            components_,
            *persistent_entities};
        for (const auto& key : candidates)
        {
            const auto found = std::ranges::find_if(
                authoring_load_result_.world_entity_ids,
                [&key](const auto actor)
                {
                    return uuids::to_string(actor.value()) == key;
                });
            if (found == authoring_load_result_.world_entity_ids.end())
                continue;
            const auto index = static_cast<std::size_t>(std::distance(
                authoring_load_result_.world_entity_ids.begin(), found));
            if (index >= authoring_load_result_.created_entities.size())
                continue;
            const auto entity = authoring_load_result_.created_entities[index];
            if (!registry.valid(entity))
                continue;

            const auto descriptor_found =
                materialized_actor_descriptors_.find(key);
            const auto* descriptor = descriptor_found ==
                    materialized_actor_descriptors_.end()
                ? nullptr
                : &descriptor_found->second;
            if (!descriptor)
            {
                dirty_actor_ids_.insert(key);
                continue;
            }
            auto document = adapter.capture(
                registry,
                entity,
                world_source_->world,
                "proxy eviction validation");
            if (document)
            {
                document->actor_class = descriptor->actor_class;
                document->space = descriptor->space;
                document->position = descriptor->position;
                document->data_layers = descriptor->data_layers;
                document->references = descriptor->references;
            }
            auto encoded = document
                ? lux::authoring::encodeWorldActorDocument(*document)
                : decltype(lux::authoring::encodeWorldActorDocument({})){};
            if (!encoded)
            {
                dirty_actor_ids_.insert(key);
                continue;
            }
            const auto digest = lux::cxx::algorithm::Sha256::hash(*encoded);
            if (lux::authoring::makeWorldActorDocumentPath(
                    lux::authoring::WorldActorId{found->value()},
                    digest) !=
                    descriptor->document_path)
            {
                dirty_actor_ids_.insert(key);
                continue;
            }

            selection_.releaseProxy(entity);
            registry.destroy(entity);
            authoring_load_result_.world_entity_ids.erase(
                authoring_load_result_.world_entity_ids.begin() +
                static_cast<std::ptrdiff_t>(index));
            authoring_load_result_.created_entities.erase(
                authoring_load_result_.created_entities.begin() +
                static_cast<std::ptrdiff_t>(index));
            materialized_actor_ids_.erase(key);
            materialized_actor_descriptors_.erase(key);
        }
        trimAuthoringDescriptorPageCache();
    }

    // -------------------------------------------------------------------------
