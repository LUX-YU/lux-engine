    EditorScene::buildEntityScenePlayCookJob(
        const std::filesystem::path& root_document)
    {
        if (!live_ || !runtime_)
            return {};

        auto job = std::make_shared<EntityScenePlayCookJob>();
        job->root_document = root_document;
        job->source_root_document = scene_path_;
        job->component_schemas.assign(
            components_.all().begin(),
            components_.all().end());
        job->asset_vfs = assets_.vfs();

        const bool is_2d = isPlanar2D();
        auto source = world_source_
            ? *world_source_
            : lux::authoring::makeWorldSourceDocument(
                  is_2d
                      ? lux::authoring::EPartitionTopology::PLANAR_XY
                      : lux::authoring::EPartitionTopology::PLANAR_XZ);
        // Editor scaffolding (viewport camera, reference grid) is NOT content.
        // The writer no longer knows what that means — it used to test
        // EditorTransientComponent itself, which is exactly what kept it stuck
        // inside the editor target. Now the host supplies the rule. Forgetting
        // this line does not fail loudly: it silently starts writing the
        // editor camera into every save, one more copy each time.

        std::unordered_map<
            std::string,
            lux::authoring::WorldActorSourceDescriptor> previous;
        std::size_t previous_count = materialized_actor_descriptors_.size();
        for (const auto& [_, cached] : world_descriptor_pages_)
            previous_count += cached.document.actors.size();
        previous.reserve(previous_count);
        for (const auto& [_, cached] : world_descriptor_pages_)
        for (const auto& actor : cached.document.actors)
        {
            previous.emplace(
                uuids::to_string(actor.id.value()),
                actor);
        }
        for (const auto& [key, descriptor] :
             materialized_actor_descriptors_)
        {
            previous.insert_or_assign(key, descriptor);
        }
        const auto original_descriptor_references =
            source.descriptor_pages;
        std::unordered_map<
            std::string,
            lux::authoring::WorldDescriptorPageDocument> descriptor_pages;
        descriptor_pages.reserve(world_descriptor_pages_.size());
        for (const auto& [_, cached] : world_descriptor_pages_)
        {
            auto page = cached.document;
            std::erase_if(
                page.actors,
                [this](const auto& actor)
                {
                    return materialized_actor_ids_.contains(
                        uuids::to_string(actor.id.value()));
                });
            descriptor_pages.emplace(
                uuids::to_string(page.id), std::move(page));
        }

        auto* const persistent_entities =
            scenePersistentEntities(runtime_.get());
        if (!persistent_entities)
        {
            std::fprintf(
                stderr,
                "[EditorScene] SceneRuntime persistent entity index is unavailable\n");
            return {};
        }
        auto& registry = runtime_->world().registry();
        std::unordered_set<std::string> actor_ids;
        std::vector<detail::WorldActorComponentSchemaSnapshot>
            rewritten_actor_schemas;
        for (const auto entity : registry.view<entt::entity>())
        {
            if (skipEditorTransient(registry, entity))
                continue;

            auto actor_document = serializeWorldActor(
                components_,
                *persistent_entities,
                registry,
                entity,
                source.world,
                "external Actor");
            if (!actor_document)
            {
                std::fprintf(
                    stderr,
                    "[EditorScene] Actor capture failed: %s\n",
                    actor_document.error().c_str());
                return {};
            }
            const auto* stable = registry.try_get<
                lux::ecs::PersistentEntityIdComponent>(entity);
            if (!stable || stable->id().empty() ||
                !actor_ids.insert(
                    uuids::to_string(stable->id().value())).second)
            {
                std::fprintf(
                    stderr,
                    "[EditorScene] Actor identity is missing or duplicate\n");
                return {};
            }

            lux::authoring::WorldActorSourceDescriptor descriptor;
            const auto old = previous.find(
                uuids::to_string(stable->id().value()));
            if (old != previous.end())
                descriptor = old->second;
            const auto previous_transform_parent =
                descriptor.transform_parent;
            descriptor.id = lux::authoring::WorldActorId{
                stable->id().value()};
            descriptor.actor_class = descriptor.actor_class.empty()
                ? "org.lux.actor"
                : descriptor.actor_class;
            if (const auto* name =
                    registry.try_get<lux::ecs::NameComponent>(entity))
            {
                descriptor.display_name = name->name;
            }
            if (descriptor.display_name.empty())
            {
                descriptor.display_name =
                    "Actor " + uuids::to_string(stable->id().value());
            }

            descriptor.position = actor_document->position;

            const auto topology_matches = [&](const auto& space)
            {
                return is_2d
                    ? space.topology ==
                          lux::authoring::EPartitionTopology::PLANAR_XY
                    : space.topology !=
                          lux::authoring::EPartitionTopology::PLANAR_XY;
            };
            const auto current_space = std::ranges::find_if(
                source.spaces,
                [&](const auto& space)
                {
                    return space.id == descriptor.space &&
                        topology_matches(space);
                });
            if (current_space == source.spaces.end())
            {
                const auto fallback = std::ranges::find_if(
                    source.spaces,
                    topology_matches);
                if (fallback == source.spaces.end())
                    return {};
                descriptor.space = fallback->id;
            }
            descriptor.transform_parent =
                actor_document->transform_parent;
            if (previous_transform_parent != descriptor.transform_parent)
            {
                if (previous_transform_parent)
                {
                    std::erase_if(
                        descriptor.references,
                        [&](const auto& reference)
                        {
                            return reference.target ==
                                    *previous_transform_parent &&
                                reference.kind == lux::authoring::
                                    EWorldActorReferenceKind::LOCAL;
                        });
                }
                if (descriptor.transform_parent)
                {
                    const auto reference = std::ranges::find(
                        descriptor.references,
                        *descriptor.transform_parent,
                        &lux::authoring::WorldActorSourceReference::target);
                    if (reference == descriptor.references.end())
                    {
                        descriptor.references.push_back({
                            *descriptor.transform_parent,
                            lux::authoring::
                                EWorldActorReferenceKind::LOCAL});
                    }
                    else
                    {
                        reference->kind = lux::authoring::
                            EWorldActorReferenceKind::LOCAL;
                    }
                }
            }
            actor_document->actor_class = descriptor.actor_class;
            actor_document->space = descriptor.space;
            actor_document->position = descriptor.position;
            actor_document->transform_parent =
                descriptor.transform_parent;
            actor_document->data_layers = descriptor.data_layers;
            actor_document->references = descriptor.references;
            detail::WorldActorComponentSchemaSnapshot schema_snapshot;
            schema_snapshot.actor = actor_document->actor;
            schema_snapshot.schemas.reserve(actor_document->components.size());
            for (const auto& component : actor_document->components)
                schema_snapshot.schemas.push_back(component.schema_name);
            rewritten_actor_schemas.push_back(std::move(schema_snapshot));
            auto encoded = lux::authoring::encodeWorldActorDocument(
                *actor_document);
            if (!encoded)
            {
                std::fprintf(
                    stderr,
                    "[EditorScene] Actor encode failed: %s\n",
                    encoded.error().c_str());
                return {};
            }
            const auto content_digest = lux::cxx::algorithm::Sha256::hash(*encoded);
            descriptor.content_digest = content_digest;
            descriptor.document_path =
                lux::authoring::makeWorldActorDocumentPath(
                    descriptor.id,
                    content_digest);
            job->documents.push_back(EntitySceneCookDocument{
                descriptor.document_path,
                std::move(*encoded)});
            const auto macro = descriptorMacro(
                source, descriptor.space, descriptor.position);
            if (!macro)
                return {};
            const auto descriptor_page_id =
                lux::authoring::makeWorldDescriptorPageId(
                    source.world, descriptor.space, *macro);
            const auto page_key = uuids::to_string(descriptor_page_id);
            auto [page, inserted] = descriptor_pages.try_emplace(page_key);
            if (inserted)
            {
                const auto old_reference = std::ranges::find(
                    original_descriptor_references,
                    descriptor_page_id,
                    &lux::authoring::WorldDescriptorPageReference::id);
                if (old_reference != original_descriptor_references.end() &&
                    !scene_path_.empty())
                {
                    auto existing =
                        lux::authoring::loadWorldDescriptorPage(
                            scene_path_, source, *old_reference);
                    if (!existing)
                        return {};
                    page->second = std::move(*existing);
                }
                else
                {
                    page->second.world = source.world;
                    page->second.id = descriptor_page_id;
                    page->second.space = descriptor.space;
                    page->second.macro = *macro;
                }
            }
            page->second.actors.push_back(std::move(descriptor));
        }

        std::vector<std::string> dirty_instances(
            dirty_instance_pages_.begin(),
            dirty_instance_pages_.end());
        std::ranges::sort(dirty_instances);
        for (const auto& instance_key : dirty_instances)
        {
            const auto authored_page = authoring_instance_clusters_.find(
                instance_key);
            if (authored_page == authoring_instance_clusters_.end())
            {
                std::fprintf(
                    stderr,
                    "[EditorScene] Dirty Instance Page is not materialized\n");
                return {};
            }
            auto encoded = lux::authoring::encodeWorldInstancePage(
                source, authored_page->second.page);
            if (!encoded)
            {
                std::fprintf(
                    stderr,
                    "[EditorScene] Instance Page encode failed: %s\n",
                    encoded.error().c_str());
                return {};
            }
            const auto digest = lux::cxx::algorithm::Sha256::hash(*encoded);
            const auto relative_path =
                lux::authoring::makeWorldInstancePagePath(
                    authored_page->second.page.instance_set,
                    authored_page->second.page.cell,
                    digest);

            const auto descriptor_page_key = uuids::to_string(
                authored_page->second.descriptor_page);
            auto container = descriptor_pages.find(descriptor_page_key);
            if (container == descriptor_pages.end())
            {
                const auto reference = std::ranges::find(
                    original_descriptor_references,
                    authored_page->second.descriptor_page,
                    &lux::authoring::WorldDescriptorPageReference::id);
                if (reference == original_descriptor_references.end() ||
                    scene_path_.empty())
                {
                    std::fprintf(
                        stderr,
                        "[EditorScene] Instance Descriptor Page is absent\n");
                    return {};
                }
                auto loaded = lux::authoring::loadWorldDescriptorPage(
                    scene_path_, source, *reference);
                if (!loaded)
                {
                    std::fprintf(
                        stderr,
                        "[EditorScene] Instance Descriptor Page load failed\n");
                    return {};
                }
                container = descriptor_pages.emplace(
                    descriptor_page_key,
                    std::move(*loaded)).first;
            }
            const auto descriptor = std::ranges::find(
                container->second.pages,
                authored_page->second.descriptor.id,
                &lux::authoring::WorldPageSourceDescriptor::id);
            if (descriptor == container->second.pages.end())
            {
                std::fprintf(
                    stderr,
                    "[EditorScene] Instance Page descriptor is absent\n");
                return {};
            }
            descriptor->document_path = relative_path;
            descriptor->content_digest = digest;
            job->documents.push_back(EntitySceneCookDocument{
                relative_path,
                std::move(*encoded)});
        }

        std::vector<std::string> dirty_terrain(
            dirty_terrain_pages_.begin(),
            dirty_terrain_pages_.end());
        std::ranges::sort(dirty_terrain);
        for (const auto& terrain_key : dirty_terrain)
        {
            const auto authored_page = terrain_pages_.find(terrain_key);
            if (authored_page == terrain_pages_.end())
            {
                std::fprintf(
                    stderr,
                    "[EditorScene] Dirty Terrain Page is not materialized\n");
                return {};
            }
            auto encoded = lux::authoring::encodeWorldTerrainPage(
                source, authored_page->second);
            if (!encoded)
            {
                std::fprintf(
                    stderr,
                    "[EditorScene] Terrain Page encode failed: %s\n",
                    encoded.error().c_str());
                return {};
            }
            const auto digest = lux::cxx::algorithm::Sha256::hash(*encoded);
            const auto relative_path =
                lux::authoring::makeWorldTerrainPagePath(
                    authored_page->second.terrain_set,
                    authored_page->second.cell,
                    digest);
            lux::authoring::WorldPageSourceDescriptor* content = nullptr;
            for (auto& [_, descriptor_page] : descriptor_pages)
            {
                const auto found = std::ranges::find_if(
                    descriptor_page.pages,
                    [&](const auto& candidate)
                    {
                        return candidate.kind == lux::authoring::
                                EWorldPageSourceKind::TERRAIN &&
                            candidate.owner == lux::authoring::
                                WorldPageSourceOwner{
                                    authored_page->second.terrain_set} &&
                            candidate.space == authored_page->second.space &&
                            candidate.cell == authored_page->second.cell;
                    });
                if (found != descriptor_page.pages.end())
                {
                    if (content != nullptr)
                    {
                        std::fprintf(
                            stderr,
                            "[EditorScene] Terrain Page identity is ambiguous\n");
                        return {};
                    }
                    content = &*found;
                }
            }
            if (!content)
            {
                std::fprintf(
                    stderr,
                    "[EditorScene] Terrain Page descriptor is absent\n");
                return {};
            }
            content->document_path = relative_path;
            content->content_digest = digest;
            job->documents.push_back(EntitySceneCookDocument{
                relative_path,
                std::move(*encoded)});
        }
        source.descriptor_pages.clear();
        for (const auto& reference : original_descriptor_references)
        {
            if (!descriptor_pages.contains(uuids::to_string(reference.id)))
                source.descriptor_pages.push_back(reference);
        }
        job->descriptor_pages.reserve(descriptor_pages.size());
        for (auto& [key, page] : descriptor_pages)
        {
            auto encoded = lux::authoring::encodeWorldDescriptorPage(
                source, page);
            if (!encoded)
            {
                std::fprintf(
                    stderr,
                    "[EditorScene] Descriptor Page encode failed: %s\n",
                    encoded.error().c_str());
                return {};
            }
            const auto digest = lux::cxx::algorithm::Sha256::hash(*encoded);
            const auto relative_path =
                lux::authoring::makeWorldDescriptorPagePath(
                    page.id, digest);
            source.descriptor_pages.push_back({
                page.id,
                page.space,
                page.macro,
                relative_path,
                digest,
                static_cast<std::uint32_t>(page.actors.size()),
                static_cast<std::uint32_t>(page.pages.size())});
            job->documents.push_back(EntitySceneCookDocument{
                relative_path,
                std::move(*encoded)});
            job->descriptor_pages.push_back(std::move(page));
        }

        auto persisted_schemas = detail::collectFinalWorldComponentSchemas(
            scene_path_,
            source,
            job->descriptor_pages,
            rewritten_actor_schemas);
        if (!persisted_schemas)
        {
            std::fprintf(
                stderr,
                "[EditorScene] Persistent component schema scan failed: %s\n",
                persisted_schemas.error().c_str());
            return {};
        }
        auto persistence = runtime_->persistenceSnapshot(*persisted_schemas);
        source.required_extensions.clear();
        source.required_extensions.reserve(
            persistence.required_extensions.size());
        for (const auto& requirement : persistence.required_extensions)
        {
            source.required_extensions.push_back({
                lux::extensions::ExtensionId{
                    std::string{requirement.id.name()}},
                requirement.required_major,
                requirement.minimum_minor});
        }

        job->source = std::move(source);
        return job;
    }

    bool EditorScene::saveTo(const std::filesystem::path& file)
    {
        const auto job = buildEntityScenePlayCookJob(file);
        if (!job)
            return false;

        // Save As must copy the complete immutable content closure. Preserving
        // an LXWA reference without copying its content-addressed object makes
        // the new root look valid but fail only when that unloaded Macro is
        // visited later.
        const auto old_root = scene_path_.empty()
            ? std::filesystem::path{}
            : std::filesystem::absolute(scene_path_).lexically_normal();
        const auto new_root =
            std::filesystem::absolute(file).lexically_normal();
        if (!old_root.empty() && old_root != new_root && world_source_)
        {
            std::unordered_set<std::string> authored;
            authored.reserve(job->documents.size());
            for (const auto& document : job->documents)
                authored.insert(document.relative_path);

            const auto copy_relative = [&](std::string_view relative) -> bool
            {
                if (authored.contains(std::string{relative}))
                    return true;
                auto source = lux::authoring::resolveWorldSourceDocument(
                    scene_path_, relative);
                if (!source)
                    return false;
                auto bytes = readDocumentBytes(*source);
                if (!bytes)
                    return false;
                auto saved = lux::authoring::saveWorldSourceDocument(
                    file, relative, *bytes);
                if (!saved)
                    return false;
                authored.insert(std::string{relative});
                return true;
            };

            const auto copy_page_children = [&](
                const lux::authoring::WorldDescriptorPageDocument& page)
            {
                for (const auto& actor : page.actors)
                {
                    if (!copy_relative(actor.document_path))
                        return false;
                }
                for (const auto& content : page.pages)
                {
                    if (!copy_relative(content.document_path))
                        return false;
                }
                return true;
            };
            for (const auto& page : job->descriptor_pages)
            {
                if (!copy_page_children(page))
                {
                    std::fprintf(stderr,
                        "[EditorScene] Save As content closure copy failed\n");
                    return false;
                }
            }
            for (const auto& reference : job->source.descriptor_pages)
            {
                if (authored.contains(reference.document_path))
                    continue;
                const auto old_reference = std::ranges::find(
                    world_source_->descriptor_pages,
                    reference.id,
                    &lux::authoring::WorldDescriptorPageReference::id);
                if (old_reference == world_source_->descriptor_pages.end())
                    continue;
                auto page = lux::authoring::loadWorldDescriptorPage(
                    scene_path_, *world_source_, *old_reference);
                if (!page || !copy_relative(reference.document_path) ||
                    !copy_page_children(*page))
                {
                    std::fprintf(stderr,
                        "[EditorScene] Save As unloaded content copy failed\n");
                    return false;
                }
            }
        }
        for (const auto& document : job->documents)
        {
            if (auto saved = lux::authoring::saveWorldSourceDocument(
                    file,
                    document.relative_path,
                    document.bytes);
                !saved)
            {
                std::fprintf(
                    stderr,
                    "[EditorScene] Actor write failed: %s\n",
                    saved.error().c_str());
                return false;
            }
        }
        if (auto saved = lux::authoring::saveWorldSource(file, job->source);
            !saved)
        {
            std::fprintf(stderr, "[EditorScene] save failed: %s\n",
                         saved.error().c_str());
            return false;
        }
        world_source_ = std::make_unique<
            lux::authoring::WorldSourceDocument>(job->source);
        world_descriptor_pages_.clear();
        materialized_actor_descriptors_.clear();
        descriptor_page_resident_bytes_ = 0u;
        descriptor_page_cache_clock_ = 0u;
        for (const auto& page : job->descriptor_pages)
        {
            for (const auto& descriptor : page.actors)
            {
                const auto key = uuids::to_string(descriptor.id.value());
                if (materialized_actor_ids_.contains(key))
                {
                    materialized_actor_descriptors_.insert_or_assign(
                        key,
                        descriptor);
                }
            }
            cacheAuthoringDescriptorPage(page);
        }
        trimAuthoringDescriptorPageCache();
        materialized_actor_ids_.clear();
        dirty_actor_ids_.clear();
        dirty_instance_pages_.clear();
        dirty_terrain_pages_.clear();
        for (auto& [_, instance_page] : authoring_instance_clusters_)
        {
            for (const auto& descriptor_page : job->descriptor_pages)
            {
                if (descriptor_page.id != instance_page.descriptor_page)
                    continue;
                const auto descriptor = std::ranges::find(
                    descriptor_page.pages,
                    instance_page.descriptor.id,
                    &lux::authoring::WorldPageSourceDescriptor::id);
                if (descriptor != descriptor_page.pages.end())
                    instance_page.descriptor = *descriptor;
                break;
            }
        }
        for (const auto entity : runtime_->world().registry().view<
                 lux::ecs::PersistentEntityIdComponent>())
        {
            if (skipEditorTransient(runtime_->world().registry(), entity))
                continue;
            const auto& stable = runtime_->world().registry().get<
                lux::ecs::PersistentEntityIdComponent>(entity);
            if (!stable.id().empty())
            {
                materialized_actor_ids_.insert(
                    uuids::to_string(stable.id().value()));
            }
        }
        if (!play_cache_root_.empty())
        {
            const auto index_path =
                lux::authoring::worldDescriptorIndexCachePath(
                    play_cache_root_, job->source.world);
            if (world_descriptor_index_)
            {
                if (!world_descriptor_index_->updatePages(
                        job->source, job->descriptor_pages))
                {
                    world_descriptor_index_.reset();
                }
            }
            else
            {
                auto index = lux::authoring::WorldDescriptorIndex::fromPages(
                    job->source, job->descriptor_pages, index_path);
                if (index)
                {
                    world_descriptor_index_ = std::make_unique<
                        lux::authoring::WorldDescriptorIndex>(
                            std::move(*index));
                }
            }
        }
        scene_path_ = file;   // successful save re-binds the scene to the file
        if (!editor_async_.collectWorldSourceGarbage(
                CollectWorldSourceGarbageOperation{
                    .world_file = scene_path_},
                [](lux::async::OperationOutcome<
                       CollectWorldSourceGarbageOperation> outcome) noexcept
                {
                    if (outcome && outcome->error.empty())
                        return;
                    const std::string message = outcome
                        ? outcome->error
                        : "operation did not complete successfully";
                    std::fprintf(
                        stderr,
                        "[EditorScene] delayed World source GC failed: %s\n",
                        message.c_str());
                }))
        {
            std::fprintf(
                stderr,
                "[EditorScene] delayed World source GC was not admitted\n");
        }
        return true;
    }
