    // Generates LXWA v4 input and cooks only LXSC/LXES runtime content.
    [[nodiscard]] int generate(const Options& options)
    {
        std::error_code error;
        fs::create_directories(options.output / "Content", error);
        fs::create_directories(options.output / "Worlds", error);
        fs::create_directories(options.output / "Derived" / options.platform,
            error);
        fs::create_directories(options.output / "Reports" / "baseline", error);
        fs::create_directories(options.output / "Licenses", error);
        if (error)
        {
            std::fprintf(stderr, "cannot create output directories: %s\n",
                error.message().c_str());
            return 1;
        }

        const auto recipe_namespace = uuids::uuid::from_string(
            "da67c8ec-66d4-5ee8-9990-4fd8f642fdd0").value();
        const auto world_uuid = named(
            recipe_namespace,
            "org.lux.benchmark.world:" + std::to_string(options.seed));
        const lux::authoring::WorldId world{world_uuid};
        const lux::authoring::PartitionSpaceId space{
            named(world_uuid, "space:surface")};
        const lux::authoring::InstanceSetId instance_set{
            named(world_uuid, "instances:environment")};
        const lux::authoring::TerrainSetId terrain_set{
            named(world_uuid, "terrain:surface")};
        const auto sky_texture_id = named(
            world_uuid, "texture:benchmark-sky-gradient");
        auto sky_texture_image = benchmarkSkyTexture(sky_texture_id);
        if (!sky_texture_image || !writeBytes(
                options.output / "Content" / "benchmark-sky.luxasset",
                *sky_texture_image))
        {
            std::fprintf(stderr, "cannot encode benchmark Sky texture\n");
            return 1;
        }

        lux::authoring::WorldSourceDocument root;
        root.world = world;
        root.spaces.push_back({
            space,
            lux::authoring::EPartitionTopology::PLANAR_XZ,
            1024.0f,
            32u});

        const auto cell_count = static_cast<std::uint64_t>(
            options.scale.terrain_edge) * options.scale.terrain_edge;
        const auto instance_count = cell_count *
            options.scale.instances_per_cell;
        root.instance_sets.push_back({instance_set, instance_count + 1u});

        constexpr std::uint32_t mesh_count = 32u;
        std::vector<std::pair<uuids::uuid, std::vector<std::byte>>> meshes;
        meshes.reserve(mesh_count + 1u);
        for (std::uint32_t variant = 0u; variant < mesh_count; ++variant)
        {
            const auto id = named(
                world_uuid, "mesh:" + std::to_string(variant));
            auto encoded = lux::asset::MeshSerDeser::encodeData(
                id, benchmarkMesh(variant));
            if (!encoded)
            {
                std::fprintf(stderr, "cannot encode benchmark Mesh %u\n",
                    variant);
                return 1;
            }
            const auto content_path = options.output / "Content" /
                ("benchmark-mesh-" + std::to_string(variant) + ".luxasset");
            if (!writeBytes(content_path, *encoded))
                return 1;
            meshes.emplace_back(id, std::move(*encoded));
        }
        const auto skeletal_mesh_id = named(
            world_uuid, "mesh:benchmark-one-bone-skinned");
        auto skeletal_mesh = benchmarkMesh(3u);
        skeletal_mesh.lods.clear();
        for (auto& source_vertex : skeletal_mesh.vertices)
        {
            std::ranges::fill(source_vertex.bone.bone_ids, 0);
            std::ranges::fill(source_vertex.bone.weights, 0.0f);
            source_vertex.bone.weights[0] = 1.0f;
        }
        auto skeletal_mesh_image = lux::asset::MeshSerDeser::encodeData(
            skeletal_mesh_id, skeletal_mesh);
        if (!skeletal_mesh_image)
        {
            std::fprintf(stderr, "cannot encode benchmark skeletal Mesh\n");
            return 1;
        }
        if (!writeBytes(
                options.output / "Content" /
                    "benchmark-skeletal-mesh.luxasset",
                *skeletal_mesh_image))
        {
            return 1;
        }
        meshes.emplace_back(
            skeletal_mesh_id, std::move(*skeletal_mesh_image));

        const auto skeleton_id = named(
            world_uuid, "skeleton:benchmark-one-bone");
        lux::rdesc::Skeleton skeleton;
        lux::rdesc::Bone_t root_bone;
        root_bone.name = "root";
        root_bone.parent_index = -1;
        root_bone.bind_local = Eigen::Affine3f::Identity();
        root_bone.inv_bind_world = Eigen::Affine3f::Identity();
        skeleton.bones.push_back(std::move(root_bone));
        auto skeleton_image = lux::asset::SkeletonSerDeser::encodeData(
            skeleton_id, skeleton);
        if (!skeleton_image || !writeBytes(
                options.output / "Content" /
                    "benchmark-skeleton.luxasset",
                *skeleton_image))
        {
            std::fprintf(stderr, "cannot encode benchmark Skeleton\n");
            return 1;
        }
        std::array<uuids::uuid, 1u + lux::asset::kBuiltinEmissiveCount>
            benchmark_materials{};
        benchmark_materials[0] = uuids::uuid::from_string(
            lux::asset::kBuiltinWhitePbrMaterialIdStr).value();
        for (std::size_t index = 0u;
             index < lux::asset::kBuiltinEmissiveCount; ++index)
        {
            benchmark_materials[index + 1u] = uuids::uuid::from_string(
                lux::asset::kBuiltinEmissiveIdStrs[index]).value();
        }
        constexpr std::array<std::array<float, 3u>, 9u>
            showcase_material_colors{{
                {0.055f, 0.065f, 0.075f}, // asphalt
                {0.55f, 0.12f, 0.08f},   // brick
                {0.72f, 0.38f, 0.08f},   // ochre
                {0.10f, 0.28f, 0.62f},   // blue
                {0.16f, 0.50f, 0.18f},   // green
                {0.55f, 0.45f, 0.12f},   // mustard
                {0.42f, 0.08f, 0.12f},   // wine
                {0.22f, 0.38f, 0.62f},   // slate
                {0.38f, 0.30f, 0.08f}    // umber
            }};
        std::array<uuids::uuid, showcase_material_colors.size()>
            showcase_materials{};
        for (std::size_t index = 0u;
             index < showcase_materials.size(); ++index)
        {
            showcase_materials[index] = named(
                world_uuid,
                "material:showcase:" + std::to_string(index));
        }
        std::vector<BenchmarkAssetImage> builtin_assets;
        builtin_assets.push_back({
            sky_texture_id,
            lux::asset::EAssetType::TEXTURE,
            "Benchmark/Texture/SkyGradient",
            std::move(*sky_texture_image)});
        builtin_assets.push_back({
            skeleton_id,
            lux::asset::EAssetType::SKELETON,
            "Benchmark/Skeleton/" + uuids::to_string(skeleton_id),
            std::move(*skeleton_image)});
        {
            const auto white_path = options.engine_content / "Materials" /
                "M_WhitePbr.luxasset";
            const auto white_image = readBytes(white_path);
            if (!white_image)
            {
                std::fprintf(
                    stderr,
                    "cannot read canonical white PBR material: %s\n",
                    white_path.string().c_str());
                return 1;
            }
            auto white_data = lux::asset::MaterialSerDeser::decodeData(
                white_image->data(), white_image->size());
            if (!white_data || !*white_data ||
                (*white_data)->parameter_count == 0u)
            {
                std::fprintf(
                    stderr,
                    "cannot decode canonical white PBR material: %s\n",
                    white_path.string().c_str());
                return 1;
            }

            auto material_manager = std::make_shared<
                lux::asset::AssetManager>(
                    lux::asset::runtimeAssetCodecCatalog());
            lux::asset::MaterialSerDeser material_codec{
                material_manager};
            for (std::size_t index = 0u;
                 index < showcase_materials.size(); ++index)
            {
                auto data = std::make_unique<lux::asset::MaterialData>(
                    **white_data);
                data->parameter_defaults[0] = {
                    showcase_material_colors[index][0],
                    showcase_material_colors[index][1],
                    showcase_material_colors[index][2],
                    0.0f};
                const auto display_name =
                    "BenchmarkShowcase" + std::to_string(index);
                auto asset = std::make_unique<lux::asset::MaterialAsset>(
                    makeAssetInfo(
                        showcase_materials[index],
                        lux::asset::EAssetType::MATERIAL,
                        display_name),
                    std::move(data));
                if (!material_manager->registerAsset(std::move(asset)))
                {
                    std::fprintf(
                        stderr,
                        "cannot register benchmark showcase material %zu\n",
                        index);
                    return 1;
                }
                const auto path = options.output / "Content" /
                    ("benchmark-material-" + std::to_string(index) +
                        ".luxasset");
                if (material_codec.exportAsLuxAsset(
                        showcase_materials[index], path) !=
                        lux::asset::EAssetError::SUCCESS)
                {
                    std::fprintf(
                        stderr,
                        "cannot encode benchmark showcase material %zu\n",
                        index);
                    return 1;
                }
                auto image = readBytes(path);
                if (!image)
                    return 1;
                builtin_assets.push_back({
                    showcase_materials[index],
                    lux::asset::EAssetType::MATERIAL,
                    "Benchmark/Material/Showcase" +
                        std::to_string(index),
                    std::move(*image)});
            }
        }
        CanonicalBenchmarkAssets cc0_assets;
        if (options.cc0_instance_percent != 0u)
        {
            auto loaded = loadCanonicalBenchmarkAssets(options.cc0_content);
            if (!loaded)
                return 1;
            cc0_assets = std::move(*loaded);
            for (std::size_t index = 0u;
                 index < kCanonicalCc0Assets.size(); ++index)
            {
                const auto destination = options.output / "Content" / "CC0" /
                    fs::path{kCanonicalCc0Assets[index].relative_path};
                std::error_code path_error;
                const auto source_path = fs::weakly_canonical(
                    options.cc0_content /
                        fs::path{kCanonicalCc0Assets[index].relative_path},
                    path_error);
                const auto destination_path = fs::weakly_canonical(
                    destination, path_error);
                if (path_error || source_path != destination_path)
                {
                    if (!writeBytes(
                            destination,
                            cc0_assets.images[index].image))
                    {
                        return 1;
                    }
                }
            }
        }
        if (!options.authoring_only)
        {
            const auto addMaterial = [&](
                const uuids::uuid& id,
                std::string filename) -> bool
            {
                auto image = readBytes(
                    options.engine_content / "Materials" / filename);
                if (!image)
                {
                    std::fprintf(
                        stderr,
                        "missing canonical benchmark material: %s\n",
                        (options.engine_content / "Materials" / filename)
                            .string().c_str());
                    return false;
                }
                builtin_assets.push_back({
                    id,
                    lux::asset::EAssetType::MATERIAL,
                    "Benchmark/Material/" +
                        fs::path{filename}.stem().string(),
                    std::move(*image)});
                return true;
            };
            if (!addMaterial(benchmark_materials[0], "M_WhitePbr.luxasset"))
                return 1;
            for (std::size_t index = 0u;
                 index < lux::asset::kBuiltinEmissiveCount; ++index)
            {
                if (!addMaterial(
                        benchmark_materials[index + 1u],
                        "M_Emissive_" + std::to_string(index) +
                            ".luxasset"))
                {
                    return 1;
                }
            }
            if (!addMaterial(
                    uuids::uuid::from_string(
                        lux::asset::kBuiltinMissingMaterialIdStr).value(),
                    "M_Missing.luxasset"))
            {
                return 1;
            }
            builtin_assets.insert(
                builtin_assets.end(),
                std::make_move_iterator(cc0_assets.images.begin()),
                std::make_move_iterator(cc0_assets.images.end()));
        }

        std::map<PageKey, lux::authoring::WorldDescriptorPageDocument>
            descriptor_pages;
        const auto world_file = options.output / "Worlds" /
            "Benchmark.luxworld";
        std::uint64_t next_instance = 1u;
        std::uint64_t actor_ordinal = 0u;
        std::uint64_t point_light_count = 0u;
        std::uint64_t spot_light_count = 0u;
        std::uint64_t skeletal_actor_count = 0u;
        std::uint64_t dynamic_body_count = 0u;
        std::uint64_t character_count = 0u;
        constexpr std::size_t sample_count =
            static_cast<std::size_t>(lux::authoring::kWorldTerrainSampleEdge) *
            lux::authoring::kWorldTerrainSampleEdge;
        constexpr std::size_t weight_bytes = sample_count * 4u;
        constexpr std::size_t hole_bytes = (sample_count + 7u) / 8u;

        for (std::uint32_t z = 0u; z < options.scale.terrain_edge; ++z)
        {
            for (std::uint32_t x = 0u; x < options.scale.terrain_edge; ++x)
            {
                const lux::authoring::WorldCellKey cell{
                    lux::authoring::EPartitionTopology::PLANAR_XZ,
                    lux::authoring::PlanarCellCoord{
                        static_cast<std::int64_t>(x),
                        static_cast<std::int64_t>(z)}};
                const auto macro = lux::authoring::macroCoordOf(cell, 32u);
                const auto* macro_coord = macro
                    ? std::get_if<lux::authoring::PlanarMacroCoord>(
                          &macro->coordinate)
                    : nullptr;
                if (!macro_coord)
                    return 1;
                const PageKey key{macro_coord->a, macro_coord->b};
                auto [page_iterator, inserted] = descriptor_pages.try_emplace(
                    key);
                auto& descriptor = page_iterator->second;
                if (inserted)
                {
                    descriptor.world = world;
                    descriptor.space = space;
                    descriptor.macro = *macro;
                    descriptor.id = lux::authoring::makeWorldDescriptorPageId(
                        world, space, *macro);
                }

                lux::authoring::WorldTerrainPageDocument terrain;
                terrain.world = world;
                terrain.terrain_set = terrain_set;
                terrain.space = space;
                terrain.cell = cell;
                terrain.height_min = -256.0f;
                terrain.height_max = 1024.0f;
                terrain.sample_spacing = 4.0f;
                terrain.weight_layer_count = 4u;
                terrain.heights.resize(sample_count);
                terrain.weight_planes[0].assign(weight_bytes, 0u);
                terrain.weight_planes[1].assign(weight_bytes, 0u);
                terrain.holes.assign(hole_bytes, 0u);
                for (std::uint32_t sy = 0u;
                     sy < lux::authoring::kWorldTerrainSampleEdge; ++sy)
                {
                    for (std::uint32_t sx = 0u;
                         sx < lux::authoring::kWorldTerrainSampleEdge; ++sx)
                    {
                        const auto sample = static_cast<std::size_t>(sy) *
                            lux::authoring::kWorldTerrainSampleEdge + sx;
                        const double wx = static_cast<double>(x) * 1024.0 +
                            static_cast<double>(sx) * 4.0;
                        const double wz = static_cast<double>(z) * 1024.0 +
                            static_cast<double>(sy) * 4.0;
                        const auto height = terrainHeight(
                            wx, wz, options.scale.terrain_edge);
                        terrain.heights[sample] = height;
                        auto* weights = terrain.weight_planes[0].data() +
                            sample * 4u;
                        if (height < 10.0f)
                            weights[0] = 255u;
                        else if (height > 260.0f)
                            weights[2] = 255u;
                        else
                            weights[1] = 255u;
                    }
                }
                auto terrain_bytes =
                    lux::authoring::encodeWorldTerrainPage(root, terrain);
                if (!terrain_bytes)
                {
                    std::fprintf(stderr, "Terrain (%u,%u): %s\n", x, z,
                        terrain_bytes.error().c_str());
                    return 1;
                }
                const auto terrain_digest = lux::cxx::algorithm::Sha256::hash(*terrain_bytes);
                const auto terrain_path =
                    lux::authoring::makeWorldTerrainPagePath(
                        terrain_set, cell, terrain_digest);
                if (!lux::authoring::saveWorldSourceDocument(
                        world_file, terrain_path, *terrain_bytes))
                {
                    return 1;
                }
                descriptor.pages.push_back({
                    named(world_uuid, "terrain-page:" +
                        std::to_string(x) + ":" + std::to_string(z)),
                    lux::authoring::EWorldPageSourceKind::TERRAIN,
                    lux::authoring::WorldPageSourceOwner{terrain_set},
                    terrain_path,
                    space,
                    cell,
                    terrain_digest});

                lux::authoring::WorldInstancePageDocument instances;
                instances.world = world;
                instances.instance_set = instance_set;
                instances.space = space;
                instances.cell = cell;
                instances.instances.reserve(
                    options.scale.instances_per_cell);
                for (std::uint32_t local = 0u;
                     local < options.scale.instances_per_cell; ++local)
                {
                    const auto random_key = options.seed ^
                        (static_cast<std::uint64_t>(x) << 40u) ^
                        (static_cast<std::uint64_t>(z) << 24u) ^ local;
                    double wx = static_cast<double>(x) * 1024.0 +
                        8.0 + unit(random_key) * 1008.0;
                    double wz = static_cast<double>(z) * 1024.0 +
                        8.0 + unit(random_key ^ 0xabcddcba98765432ull) *
                            1008.0;
                    // Reserve a deterministic, readable town in the central
                    // 2x2 Cells. The remaining population stays stochastic and
                    // continues to provide the intended culling/residency load,
                    // while the fixed route now has streets and silhouettes a
                    // human can recognize instead of uniformly scattered boxes.
                    const auto center_delta_x = std::abs(
                        static_cast<std::int64_t>(x) * 2 + 1 -
                        static_cast<std::int64_t>(options.scale.terrain_edge));
                    const auto center_delta_z = std::abs(
                        static_cast<std::int64_t>(z) * 2 + 1 -
                        static_cast<std::int64_t>(options.scale.terrain_edge));
                    const auto water_cell = benchmarkWaterCell(
                        options.scale.terrain_edge);
                    const bool showcase_town = center_delta_x <= 1 &&
                        center_delta_z <= 1 && local < 96u &&
                        !(x == water_cell && z == water_cell);
                    const bool showcase_road = showcase_town && local < 24u;
                    if (showcase_town)
                    {
                        if (showcase_road)
                        {
                            wx = static_cast<double>(x) * 1024.0 + 420.0;
                            wz = static_cast<double>(z) * 1024.0 + 100.0 +
                                static_cast<double>(local) * 40.0;
                        }
                        else
                        {
                            const auto building = local - 24u;
                            static constexpr double x_offsets[]{
                                300.0, 370.0, 470.0, 540.0};
                            wx = static_cast<double>(x) * 1024.0 +
                                x_offsets[building % 4u];
                            wz = static_cast<double>(z) * 1024.0 + 120.0 +
                                static_cast<double>(building / 4u) * 50.0;
                        }
                    }
                    lux::authoring::EditableWorldInstance instance;
                    instance.id = {instance_set, next_instance++};
                    const auto surface_height = terrainHeight(
                        wx, wz, options.scale.terrain_edge);
                    instance.position = lux::math::Position3d{
                        wx,
                        surface_height + (showcase_road ? 0.12 : 0.0),
                        wz};
                    const auto random_variant = static_cast<std::size_t>(
                        mix(random_key ^ 0x8a5cd789635d2dffull) %
                            meshes.size());
                    const auto variant = showcase_road
                        ? std::size_t{31u}
                        : (showcase_town
                            ? static_cast<std::size_t>(
                                  (((local - 24u) % 8u) * 4u + 1u))
                            : random_variant);
                    const bool use_cc0 = !showcase_town &&
                        (mix(random_key) % 100u) <
                            options.cc0_instance_percent;
                    if (use_cc0)
                    {
                        const auto& renderable = cc0_assets.renderables[
                            mix(random_key ^ 0x2f6e2b1d9ca13d5bull) %
                            cc0_assets.renderables.size()];
                        instance.mesh = renderable.first;
                        instance.material_instance = renderable.second;
                    }
                    else
                    {
                        instance.mesh = meshes[variant].first;
                        if (showcase_town)
                        {
                            instance.material_instance = showcase_materials[
                                showcase_road
                                    ? 0u
                                    : 1u + ((local - 24u) / 4u) % 8u];
                        }
                        else
                        {
                            // Most synthetic instances use the normal lit
                            // material. A small deterministic population uses
                            // the emissive family so night routes pressure
                            // material residency and PSO switching as well.
                            const auto material_variant =
                                (random_key % 23u == 0u)
                                    ? 1u + static_cast<std::size_t>(
                                        mix(random_key) %
                                        lux::asset::kBuiltinEmissiveCount)
                                    : 0u;
                            instance.material_instance =
                                benchmark_materials[material_variant];
                        }
                    }
                    if (showcase_road)
                    {
                        constexpr double sample_distance = 6.0;
                        const auto dx = terrainHeight(
                            wx - sample_distance,
                            wz,
                            options.scale.terrain_edge) - terrainHeight(
                                wx + sample_distance,
                                wz,
                                options.scale.terrain_edge);
                        const auto dz = terrainHeight(
                            wx,
                            wz - sample_distance,
                            options.scale.terrain_edge) - terrainHeight(
                                wx,
                                wz + sample_distance,
                                options.scale.terrain_edge);
                        const Eigen::Vector3f normal = Eigen::Vector3f{
                            static_cast<float>(dx),
                            static_cast<float>(sample_distance * 2.0),
                            static_cast<float>(dz)}.normalized();
                        const Eigen::Quaternionf terrain_rotation =
                            Eigen::Quaternionf::FromTwoVectors(
                                Eigen::Vector3f::UnitY(), normal);
                        instance.rotation = {
                            terrain_rotation.x(),
                            terrain_rotation.y(),
                            terrain_rotation.z(),
                            terrain_rotation.w()};
                    }
                    else
                    {
                        const auto yaw = showcase_town
                            ? (((local - 24u) % 4u) < 2u
                                ? 1.57079632679f
                                : -1.57079632679f)
                            : static_cast<float>(
                                unit(random_key ^ 0x1122334455667788ull) *
                                3.14159265358979323846);
                        const auto half_yaw = yaw * 0.5f;
                        instance.rotation = {
                            0.0f,
                            std::sin(half_yaw),
                            0.0f,
                            std::cos(half_yaw)};
                    }
                    if (showcase_road)
                    {
                        // benchmarkMesh(31) is four metres wide before scale.
                        // Keep a recognisable 12 m carriageway while retaining
                        // the 42 m longitudinal overlap between segments.
                        instance.scale = {3.0f, 1.0f, 10.5f};
                        instance.rgba8 = 0xffffffffu;
                    }
                    else
                    {
                        const auto scale = showcase_town
                            ? 4.0f + static_cast<float>(
                                  ((local - 24u) / 4u) % 4u) * 0.65f
                            : 0.65f + static_cast<float>(
                                  unit(random_key ^ 0x8877665544332211ull) *
                                  2.4);
                        instance.scale = {scale, scale, scale};
                        instance.rgba8 = showcase_town
                            ? 0xffffffffu
                            : 0xff90a878u +
                                  static_cast<std::uint32_t>(
                                      variant * 0x00030302u);
                    }
                    instances.instances.push_back(std::move(instance));
                }
                auto instance_bytes =
                    lux::authoring::encodeWorldInstancePage(root, instances);
                if (!instance_bytes)
                {
                    std::fprintf(stderr, "Instances (%u,%u): %s\n", x, z,
                        instance_bytes.error().c_str());
                    return 1;
                }
                const auto instance_digest = lux::cxx::algorithm::Sha256::hash(*instance_bytes);
                const auto instance_path =
                    lux::authoring::makeWorldInstancePagePath(
                        instance_set, cell, instance_digest);
                if (!lux::authoring::saveWorldSourceDocument(
                        world_file, instance_path, *instance_bytes))
                {
                    return 1;
                }
                descriptor.pages.push_back({
                    named(world_uuid, "instance-page:" +
                        std::to_string(x) + ":" + std::to_string(z)),
                    lux::authoring::EWorldPageSourceKind::INSTANCE,
                    lux::authoring::WorldPageSourceOwner{instance_set},
                    instance_path,
                    space,
                    cell,
                    instance_digest});

                for (std::uint32_t local = 0u;
                     local < options.scale.actors_per_cell; ++local)
                {
                    const auto current_actor = actor_ordinal++;
                    const auto actor_uuid = named(
                        world_uuid,
                        "actor:" + std::to_string(current_actor));
                    const double wx = static_cast<double>(x) * 1024.0 +
                        64.0 + local * 9.0;
                    const double wz = static_cast<double>(z) * 1024.0 +
                        64.0 + local * 7.0;
                    lux::authoring::WorldActorDocument actor;
                    actor.world = world;
                    actor.actor =
                        lux::authoring::WorldActorId{actor_uuid};
                    actor.actor_class = "org.lux.benchmark.semantic";
                    actor.space = space;
                    actor.position = lux::math::Position3d{
                        wx,
                        terrainHeight(wx, wz, options.scale.terrain_edge),
                        wz};
                    lux::serialize::NameTable component_names;
                    addActorComponent(
                        actor,
                        component_names,
                        "lux::ecs::Transform3DComponent",
                        [](TaggedComponentFields&) {});

                    // Environment is ordinary Authoring/ECS content. Keeping
                    // it on one deterministic actor removes the former LXWA
                    // singleton while preserving the benchmark scene facts.
                    if (current_actor == 0u)
                    {
                        actor.actor_class =
                            "org.lux.benchmark.environment";
                        addActorComponent(
                            actor,
                            component_names,
                            "lux::ecs::SkyboxComponent",
                            [=](TaggedComponentFields& fields)
                            {
                                fields.asset(
                                    "equirect_texture_id", sky_texture_id);
                                fields.floating("rotation_radians", 0.0f);
                                fields.floating("intensity", 1.0f);
                            });
                        addActorComponent(
                            actor,
                            component_names,
                            "lux::ecs::DirectionalLightComponent",
                            [](TaggedComponentFields& fields)
                            {
                                fields.vec3("direction", {
                                    -0.3812946f, -0.8227936f, -0.4214309f});
                                fields.vec3("color", {1.0f, 0.94f, 0.82f});
                                fields.floating("intensity", 2.0f);
                                fields.boolean("cast_shadow", true);
                                fields.uint32("shadow_map_size", 2048u);
                                fields.uint32("cascade_count", 4u);
                            });
                        addActorComponent(
                            actor,
                            component_names,
                            "lux::ecs::HeightFogComponent",
                            [](TaggedComponentFields& fields)
                            {
                                fields.boolean("enabled", true);
                                fields.vec3("color", {0.56f, 0.64f, 0.72f});
                                fields.floating("density", 0.00011f);
                                fields.floating("start_distance", 500.0f);
                                fields.floating("reference_height", 80.0f);
                                fields.floating("height_falloff", 0.004f);
                                fields.floating("maximum_opacity", 0.88f);
                            });
                    }

                    // Spatially streamed local-light pressure. The modular
                    // ordinals keep population counts deterministic for every
                    // scale while distributing lights across the whole World.
                    if (current_actor % 12u == 0u)
                    {
                        ++point_light_count;
                        actor.actor_class =
                            "org.lux.benchmark.semantic.point-light";
                        const auto hue = static_cast<float>(
                            unit(current_actor ^ options.seed));
                        addActorComponent(
                            actor,
                            component_names,
                            "lux::ecs::PointLightComponent",
                            [=](TaggedComponentFields& fields)
                            {
                                fields.vec3("color", {
                                    0.45f + 0.55f * hue,
                                    0.55f + 0.35f * (1.0f - hue),
                                    0.72f + 0.28f * hue});
                                fields.floating("intensity", 5.0f + 9.0f * hue);
                                fields.floating("range", 85.0f + 95.0f * hue);
                                fields.boolean(
                                    "cast_shadow",
                                    current_actor % 1536u == 0u);
                                fields.uint32("shadow_map_size", 1024u);
                            });
                    }
                    else if (current_actor % 52u == 7u)
                    {
                        ++spot_light_count;
                        actor.actor_class =
                            "org.lux.benchmark.semantic.spot-light";
                        addActorComponent(
                            actor,
                            component_names,
                            "lux::ecs::SpotLightComponent",
                            [=](TaggedComponentFields& fields)
                            {
                                fields.vec3("direction", {0.22f, -0.95f, 0.22f});
                                fields.vec3("color", {1.0f, 0.62f, 0.28f});
                                fields.floating("intensity", 10.0f);
                                fields.floating("range", 180.0f);
                                fields.floating("inner_cone_angle", 0.32f);
                                fields.floating("outer_cone_angle", 0.58f);
                                fields.boolean(
                                    "cast_shadow",
                                    (current_actor / 52u) % 13u == 0u);
                            });
                    }

                    // Five hundred one-bone skinned actors in the 100x
                    // recipe exercise animation resolution, per-frame skinning
                    // batches and frozen-pose render ghosts without importing a
                    // game-specific character library.
                    if (current_actor % 20u == 1u)
                    {
                        ++skeletal_actor_count;
                        actor.actor_class =
                            "org.lux.benchmark.semantic.skeletal";
                        addActorComponent(
                            actor,
                            component_names,
                            "lux::ecs::SkeletalMeshComponent",
                            [&](TaggedComponentFields& fields)
                            {
                                fields.asset(
                                    "mesh_asset_id", skeletal_mesh_id);
                                fields.asset(
                                    "material_asset_id",
                                    benchmark_materials[0]);
                                fields.asset(
                                    "skeleton_asset_id", skeleton_id);
                                fields.boolean("cast_shadow", true);
                                fields.boolean("visible", true);
                            });
                        addActorComponent(
                            actor,
                            component_names,
                            "lux::ecs::AnimatorComponent",
                            [](TaggedComponentFields&) {});
                    }

                    // A fixed fraction of semantic Actors exercise the Jolt
                    // bridge. Character controllers are kept disjoint from
                    // rigid bodies because Jolt represents them differently.
                    if (current_actor % 160u == 3u)
                    {
                        ++character_count;
                        actor.actor_class =
                            "org.lux.benchmark.semantic.character";
                        addActorComponent(
                            actor,
                            component_names,
                            "lux::ecs::Collider3DComponent",
                            [](TaggedComponentFields& fields)
                            {
                                fields.floating("radius", 0.42f);
                                fields.floating("half_height", 0.9f);
                            });
                        addActorComponent(
                            actor,
                            component_names,
                            "lux::ecs::CharacterController3DComponent",
                            [](TaggedComponentFields& fields)
                            {
                                fields.vec3("desired_velocity", {1.2f, 0.0f, 0.4f});
                                fields.floating("maximum_slope_degrees", 50.0f);
                                fields.floating("step_height", 0.35f);
                            });
                    }
                    else if (current_actor % 5u == 2u)
                    {
                        ++dynamic_body_count;
                        actor.actor_class =
                            "org.lux.benchmark.semantic.dynamic-body";
                        addActorComponent(
                            actor,
                            component_names,
                            "lux::ecs::Collider3DComponent",
                            [](TaggedComponentFields& fields)
                            {
                                fields.vec3("half_extents", {0.45f, 0.7f, 0.45f});
                            });
                        addActorComponent(
                            actor,
                            component_names,
                            "lux::ecs::RigidBody3DComponent",
                            [=](TaggedComponentFields& fields)
                            {
                                const auto speed = static_cast<float>(
                                    unit(current_actor ^ 0xb4b82e39u));
                                fields.vec3("linear_velocity", {
                                    0.4f + speed * 2.0f, 0.0f, 0.2f});
                                fields.floating("mass", 2.0f + speed * 18.0f);
                                fields.boolean(
                                    "continuous_collision", speed > 0.85f);
                            });
                    }

                    // One finite lake surface is enough to keep the dedicated
                    // Water pass, fog integration and transition ownership in
                    // every deterministic benchmark scale.
                    const auto water_cell = benchmarkWaterCell(
                        options.scale.terrain_edge);
                    if (x == water_cell && z == water_cell && local == 0u)
                    {
                        actor.actor_class =
                            "org.lux.benchmark.environment.water";
                        actor.position = lux::math::Position3d{
                            static_cast<double>(x) * 1024.0 + 512.0,
                            terrainHeight(
                                static_cast<double>(x) * 1024.0 + 512.0,
                                static_cast<double>(z) * 1024.0 + 512.0,
                                options.scale.terrain_edge) +
                                1.5,
                            static_cast<double>(z) * 1024.0 + 512.0};
                        addActorComponent(
                            actor,
                            component_names,
                            "lux::ecs::WaterSurfaceComponent",
                            [](TaggedComponentFields& fields)
                            {
                                fields.vec2("half_extent", {300.0f, 300.0f});
                                fields.vec2("normal_scroll_a", {0.015f, 0.006f});
                                fields.vec2("normal_scroll_b", {-0.008f, 0.012f});
                                fields.vec3("absorption_color", {0.03f, 0.16f, 0.19f});
                                fields.floating("absorption_distance", 8.0f);
                                fields.floating("roughness", 0.12f);
                                fields.floating("normal_strength", 0.35f);
                                fields.floating("wave_scale", 0.08f);
                            });
                    }
                    finishActorComponents(actor, component_names);
                    auto actor_bytes =
                        lux::authoring::encodeWorldActorDocument(actor);
                    if (!actor_bytes)
                        return 1;
                    lux::authoring::WorldActorSourceDescriptor actor_desc;
                    actor_desc.id = actor.actor;
                    actor_desc.display_name = "Semantic " +
                        std::to_string(actor_ordinal);
                    actor_desc.actor_class = actor.actor_class;
                    actor_desc.content_digest = lux::cxx::algorithm::Sha256::hash(*actor_bytes);
                    actor_desc.document_path =
                        lux::authoring::makeWorldActorDocumentPath(
                            actor.actor, actor_desc.content_digest);
                    actor_desc.space = space;
                    actor_desc.position = actor.position;
                    actor_desc.bounds_half_extent = {1.0f, 2.0f, 1.0f};
                    if (!lux::authoring::saveWorldSourceDocument(
                            world_file,
                            actor_desc.document_path,
                            *actor_bytes))
                    {
                        return 1;
                    }
                    descriptor.actors.push_back(std::move(actor_desc));
                }
            }
            std::printf("generated terrain row %u/%u\n",
                z + 1u, options.scale.terrain_edge);
        }

        for (auto& [key, descriptor] : descriptor_pages)
        {
            auto encoded = lux::authoring::encodeWorldDescriptorPage(
                root, descriptor);
            if (!encoded)
            {
                std::fprintf(stderr, "Descriptor (%lld,%lld): %s\n",
                    static_cast<long long>(key.x),
                    static_cast<long long>(key.z),
                    encoded.error().c_str());
                return 1;
            }
            const auto digest = lux::cxx::algorithm::Sha256::hash(*encoded);
            const auto path = lux::authoring::makeWorldDescriptorPagePath(
                descriptor.id, digest);
            if (!lux::authoring::saveWorldSourceDocument(
                    world_file, path, *encoded))
            {
                return 1;
            }
            root.descriptor_pages.push_back({
                descriptor.id,
                descriptor.space,
                descriptor.macro,
                path,
                digest,
                static_cast<std::uint32_t>(descriptor.actors.size()),
                static_cast<std::uint32_t>(descriptor.pages.size())});
        }
        if (const auto saved = lux::authoring::saveWorldSource(
                world_file, root); !saved)
        {
            std::fprintf(stderr, "World Root: %s\n", saved.error().c_str());
            return 1;
        }

        auto project = lux::authoring::ProjectManifest::makeDefault(
            "WorldBenchmark");
        project.project_guid = named(world_uuid, "project");
        project.display_name = "lux-engine World Benchmark";
        project.default_world = "Worlds/Benchmark.luxworld";
        project.binary_name = "WorldBenchmark";
        if (const auto saved = project.saveToFile(
                options.output / "WorldBenchmark.luxproject"); !saved)
        {
            std::fprintf(stderr, "Project: %s\n", saved.error().c_str());
            return 1;
        }

        auto recipe = std::string{
            "{\n  \"version\": 4,\n  \"seed\": "} +
            std::to_string(options.seed) +
            ",\n  \"scale\": \"" + std::string{options.scale.name} +
            "\",\n  \"terrain_pages\": " + std::to_string(cell_count) +
            ",\n  \"terrain_sample_spacing_m\": 4,\n"
            "  \"terrain_page_edge_m\": 1024,\n  \"instances\": " +
            std::to_string(instance_count) +
            ",\n  \"semantic_actors\": " +
            std::to_string(actor_ordinal) +
            ",\n  \"procedural_meshes\": " +
            std::to_string(mesh_count) +
            ",\n  \"classic_lods_per_procedural_mesh\": 3" +
            ",\n  \"skeletal_meshes\": 1" +
            ",\n  \"skeletons\": 1" +
            ",\n  \"skeletal_actors\": " +
            std::to_string(skeletal_actor_count) +
            ",\n  \"point_lights\": " +
            std::to_string(point_light_count) +
            ",\n  \"spot_lights\": " +
            std::to_string(spot_light_count) +
            ",\n  \"dynamic_bodies\": " +
            std::to_string(dynamic_body_count) +
            ",\n  \"character_controllers\": " +
            std::to_string(character_count) +
            ",\n  \"cc0_assets\": " +
            std::to_string(options.cc0_instance_percent == 0u
                ? 0u
                : kCanonicalCc0Assets.size()) +
            ",\n  \"cc0_renderables\": " +
            std::to_string(cc0_assets.renderables.size()) +
            ",\n  \"cc0_instance_percent\": " +
            std::to_string(options.cc0_instance_percent) +
            ",\n  \"platform\": \"" + options.platform +
            "\",\n  \"canonical_cc0\": [\n";
        const auto canonical_cc0_count = options.cc0_instance_percent == 0u
            ? 0u
            : kCanonicalCc0Assets.size();
        for (std::size_t index = 0u;
             index < canonical_cc0_count; ++index)
        {
            const auto& asset = kCanonicalCc0Assets[index];
            recipe += "    {\"path\": \"" +
                std::string{asset.relative_path} +
                "\", \"sha256\": \"" + std::string{asset.sha256} +
                "\"}";
            recipe += index + 1u == kCanonicalCc0Assets.size()
                ? "\n"
                : ",\n";
        }
        recipe += "  ]\n}\n";
        if (!writeText(options.output / "benchmark-recipe.json", recipe))
            return 1;

        if (!options.authoring_only)
        {
            lux::meta::meta_module_init();
            lux::ecs::ComponentTypeCatalog components;
            const auto registered =
                lux::ecs::registerGeneratedComponents(components);
            if (!registered || *registered == 0u)
            {
                std::fprintf(
                    stderr,
                    "Spatial3D EntityScene cook has no component schemas\n");
                return 1;
            }
            lux::toolchain::Spatial3DMeshAssetCatalog mesh_assets;
            mesh_assets.meshes.reserve(
                meshes.size() + builtin_assets.size());
            for (const auto& [id, image] : meshes)
                mesh_assets.meshes.push_back({id, image});
            for (const auto& asset : builtin_assets)
            {
                if (asset.type == lux::asset::EAssetType::MESH)
                {
                    mesh_assets.meshes.push_back({
                        asset.id, asset.image});
                }
            }
            const auto cooked =
                lux::toolchain::cookSpatial3DEntitySceneSource(
                world_file,
                components,
                mesh_assets,
                {},
                {});
            if (!cooked)
            {
                std::fprintf(stderr, "Spatial3D EntityScene cook failed: %s\n",
                    cooked.error().detail.c_str());
                return 1;
            }
            if (!publishPak(
                    std::move(*cooked),
                    meshes,
                    builtin_assets,
                    options.output / "Derived" / options.platform /
                        "WorldBenchmark.luxpak"))
            {
                return 1;
            }
        }

        std::printf(
            "WorldBenchmark %.*s: %llu Terrain Pages, %llu Instances, "
            "%llu Actors\n",
            static_cast<int>(options.scale.name.size()),
            options.scale.name.data(),
            static_cast<unsigned long long>(cell_count),
            static_cast<unsigned long long>(instance_count),
            static_cast<unsigned long long>(actor_ordinal));
        return 0;
    }
