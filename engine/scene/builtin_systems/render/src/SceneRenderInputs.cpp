#include <lux/engine/scene/SceneRenderInputs.hpp>
#include <lux/engine/render/commands/Error.hpp>

#include <bit>

namespace lux::scene::detail
{
    std::uint64_t programHash(const rdesc::MaterialProgramSet& program) noexcept
    {
        std::uint64_t hash = 14695981039346656037ULL;
        for (const auto& use : program.programs)
        {
            for (const auto& stage : use.shader.stages)
            {
                for (const auto word : stage.words)
                {
                    hash = (hash ^ word) * 1099511628211ULL;
                }
            }
        }
        return hash;
    }

    RenderResult<VDirectLight> lightValue(const rdesc::LightDescription& light, const Eigen::Affine3d& world) noexcept
    {
        const auto position = world.translation().cast<float>().eval();
        const auto direction = (-world.linear().col(2)).normalized().cast<float>().eval();
        const std::array<float, 4> radiance{
            light.color[0] * light.intensity,
            light.color[1] * light.intensity,
            light.color[2] * light.intensity,
            0
        };
        switch (light.type)
        {
        case rdesc::ELightType::DIRECTIONAL:
            return VDirectLight{DirectionalLightGpu{radiance, {direction.x(), direction.y(), direction.z(), 0}}};
        case rdesc::ELightType::POINT:
            return VDirectLight{PointLightGpu{
                {position.x(), position.y(), position.z(), light.range},
                radiance,
                {light.attenuation_constant, light.attenuation_linear, light.attenuation_quadratic, 0}
            }};
        case rdesc::ELightType::SPOT:
            return VDirectLight{SpotLightGpu{
                {position.x(), position.y(), position.z(), light.range},
                radiance,
                {direction.x(), direction.y(), direction.z(), std::cos(light.outer_cone_angle)},
                {light.attenuation_constant,
                 light.attenuation_linear,
                 light.attenuation_quadratic,
                 std::cos(light.inner_cone_angle)}
            }};
        default:
            return cxx::unexpected(RenderError{kRuntimeUnsupported});
        }
    }

    bool bindingReady(
        const MeshBinding& binding,
        const engine::render::RenderAssets& assets,
        const SceneMaterials& materials,
        std::uint64_t source,
        RenderError& failure
    ) noexcept
    {
        const RenderAssetVersion key{binding.visual.mesh, binding.revision, source};
        const auto& material = materials.find({binding.visual.material, binding.revision, source})->second;
        failure = assets.failure(key);
        if (failure.type == 0)
        {
            failure = material.error;
        }
        return failure.type == 0 && assets.mesh(key) && !material.material.isNull();
    }

    static std::vector<RasterGeometryInput> prepareGeometryInputs(
        const SceneMeshes& meshes,
        const SceneMaterials& materials,
        const engine::render::RenderAssets& assets,
        std::uint64_t source,
        bool include_replacements
    ) noexcept
    {
        const auto assetKey = [source](asset::AssetId id, std::uint64_t revision) noexcept
        { return RenderAssetVersion{id, revision, source}; };
        std::vector<RasterGeometryInput> geometry;
        std::unordered_multimap<std::uint64_t, std::size_t> group_indices;
        std::vector<const rdesc::MaterialProgramSet*> group_programs;
        std::vector<const MeshBinding*> bindings;
        for (const auto& [entity, entry] : meshes)
        {
            if (!entry.present || entry.instance.isNull())
            {
                continue;
            }
            bindings.push_back(&entry.binding);
            RenderError failure;
            if (include_replacements && entry.replacement &&
                bindingReady(*entry.replacement, assets, materials, source, failure))
            {
                bindings.push_back(&*entry.replacement);
            }
        }
        for (const auto* binding : bindings)
        {
            const auto& mesh = *assets.mesh(assetKey(binding->visual.mesh, binding->revision));
            const auto& material = materials.find(assetKey(binding->visual.material, binding->revision))->second;
            const auto& program = assets.material(assetKey(binding->visual.material, binding->revision))->programs;
            const auto bucket = material.program_hash ^ mesh.mesh.hash();
            const auto [first, last] = group_indices.equal_range(bucket);
            auto found = first;
            for (; found != last; ++found)
            {
                const auto& group = geometry[found->second];
                if (group.selection.mesh == mesh.mesh &&
                    (group_programs[found->second] == &program || *group_programs[found->second] == program))
                {
                    break;
                }
            }
            if (found != last)
            {
                auto& group = geometry[found->second];
                const RasterGeometrySelection candidate{mesh.mesh, material.material};
                if (!(candidate < group.selection))
                {
                    continue;
                }
                // Deterministic table representative; instance iteration order is not a recipe change.
                group.selection = candidate;
                group.textures.clear();
                for (const auto texture : material.textures)
                {
                    const auto* value = texture.asset.isNull() ? nullptr : assets.texture(texture);
                    group.textures.push_back(
                        value ? value->description : ImageShape{1, 1, 1, 1, rdesc::ETextureFormat::RGBA8_UNORM}
                    );
                }
                continue;
            }
            group_indices.emplace(bucket, geometry.size());
            group_programs.push_back(&program);
            RasterGeometryInput group{mesh.geometry, program, {mesh.mesh, material.material}};
            // Keep the raster recipe's canonical winding. Camera projection is not an additional
            // source-mesh winding conversion; mirrored instance transforms are handled by the Mesh Feature.
            for (const auto texture : material.textures)
            {
                const auto* value = texture.asset.isNull() ? nullptr : assets.texture(texture);
                group.textures.push_back(
                    value ? value->description : ImageShape{1, 1, 1, 1, rdesc::ETextureFormat::RGBA8_UNORM}
                );
            }
            geometry.push_back(std::move(group));
        }
        std::ranges::sort(
            geometry,
            [](const auto& left, const auto& right) noexcept { return left.selection < right.selection; }
        );
        return geometry;
    }

    static std::vector<SceneLightInput> collectLightInputs(
        const SceneLights& lights,
        const simulation::ecs::Registry& registry
    ) noexcept
    {
        std::vector<SceneLightInput> result;
        for (const auto& [bits, entry] : lights)
        {
            const auto* source =
                registry.try_get<const simulation::ecs::Light3D>(static_cast<simulation::ecs::Entity>(bits));
            if (entry.present && !entry.light.isNull() && source)
            {
                result.push_back({entry.light, source->value});
            }
        }
        std::ranges::sort(
            result,
            [](const auto& left, const auto& right) noexcept { return left.light < right.light; }
        );
        return result;
    }

    SceneEffectInputs prepareSceneEffectInputs(
        const SceneMeshes& meshes,
        const SceneMaterials& materials,
        const SceneLights& lights,
        const simulation::ecs::Registry& registry,
        const engine::render::RenderAssets& assets,
        std::uint64_t source,
        std::uint32_t previous_capacity,
        bool include_replacements
    ) noexcept
    {
        return {
            prepareGeometryInputs(meshes, materials, assets, source, include_replacements),
            collectLightInputs(lights, registry),
            std::max(previous_capacity, std::bit_ceil(std::max<std::uint32_t>(128, std::uint32_t(meshes.size()))))
        };
    }

    RenderResult<ConfigValue> produceLitSceneInputs(const SceneEffectInputs& input, const CameraInput& camera) noexcept
    {
        return ConfigValue::make(
            LitSceneInputs{input.geometry, input.lights, input.capacity, camera.near_distance, camera.far_distance}
        );
    }
} // namespace lux::scene::detail
