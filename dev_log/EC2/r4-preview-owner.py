from pathlib import Path
import re,json
s=Path('E:/SyncForder/CodeRepos/lux-engine-ec2'); b=s/'editor/activities/material'; inc=b/'include/lux/engine/editor/material'
(inc/'MaterialPreview.hpp').write_text('''#pragma once
#include <lux/engine/editor/material/MaterialCompilation.hpp>
#include <lux/engine/editor/scene/SceneProjection.hpp>
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <any>

namespace lux::editor::material
{
    struct MaterialPreviewEnvironment final
    {
        scene::ProjectionEnvironment scene;
        std::vector<render::RenderFeatureRegistration> features;
    };
    enum class EMaterialPreviewError : std::uint8_t { BUSY, CLOSED, INVALID_INPUT, CAPACITY, PREPARATION };
    struct MaterialPreviewFailure final
    {
        EMaterialPreviewError code;
        std::string domain;
        std::any cause;
    };
    template<class T> using MaterialPreviewResult = cxx::expected<T, MaterialPreviewFailure>;
    struct PreviewAdoptionKey final
    {
        uuids::uuid target;
        std::uint64_t generation{};
        MaterialCompileInputKey input;
        std::uint64_t recipe{1}, environment{1};
        friend bool operator==(const PreviewAdoptionKey&, const PreviewAdoptionKey&) = default;
    };
    struct MaterialPreviewStatus final
    {
        PreviewAdoptionKey desired;
        std::optional<PreviewAdoptionKey> prepared, accepted;
        bool stale{};
        std::string diagnostic;
        std::optional<VMaterialCompileFailure> compilation_failure;
    };
    // One live target, with independent adoption generations and original Runtime retirement.
    class MaterialPreview final
    {
    public:
        MaterialPreview(lux::scene::SceneRuntime&, MaterialPreviewEnvironment);
        ~MaterialPreview();
        MaterialPreview(const MaterialPreview&) = delete;
        MaterialPreview& operator=(const MaterialPreview&) = delete;
        MaterialPreview(MaterialPreview&&) = delete;
        MaterialPreview& operator=(MaterialPreview&&) = delete;
        [[nodiscard]] MaterialPreviewResult<PreviewAdoptionKey> setDesired(MaterialCompileInputKey) noexcept;
        // Accept owning completion facts; never borrow task control. Stale completions only settle.
        [[nodiscard]] MaterialPreviewResult<void> receive(PreviewAdoptionKey,
            MaterialCompileResult<std::shared_ptr<const CompiledMaterial>>, lux::scene::RenderAssetInput);
        void update() noexcept;
        [[nodiscard]] MaterialPreviewResult<void> reset(lux::scene::RenderAssetInput);
        [[nodiscard]] MaterialPreviewResult<void>
        navigate(const simulation::ecs::Transform3D&, const lux::scene::Camera&);
        [[nodiscard]] MaterialPreviewStatus status() const;
        [[nodiscard]] simulation::ecs::Entity camera() const noexcept;
        [[nodiscard]] lux::scene::SceneInstanceId instance() const noexcept;
        [[nodiscard]] lux::scene::InstanceRetirement close() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
''')
p=b/'src/MaterialPreview.cpp';t=p.read_text()
# Extract pure recipe construction from the adoption loop; no second Runtime/renderer owner.
helpers=t[t.index('        template <class T> T previewIdentity'):t.index('\n    struct MaterialPreview::Impl')]
helpers=helpers[:helpers.rfind('    }')]
helpers=helpers.replace('MaterialCompileResult','MaterialPreviewResult').replace('VMaterialCompileFailure{MaterialPreviewFailure{std::move(domain), std::move(error)}}','MaterialPreviewFailure{EMaterialPreviewError::PREPARATION, std::move(domain), std::move(error)}')
start=t.index('        lux::simulation::SimulationDescriptionBuilder simulation;')
end=t.index('        lux::scene::RenderFeatureSceneBindings bindings',start)
description=t[start:end].replace('environment_.features','features').replace('preview_render_system_', 'system::SystemInstanceId{2}')
pri=b/'pinclude/lux/engine/editor/material/MaterialPreviewRecipe.hpp';pri.parent.mkdir(parents=True,exist_ok=True)
pri.write_text('''#pragma once
#include <lux/engine/editor/material/MaterialPreview.hpp>
#include <lux/engine/world/WorldDescription.hpp>
#include <lux/engine/scene/SceneDescription.hpp>
#include <lux/engine/simulation/SimulationDescription.hpp>
#include <lux/engine/description/Light.hpp>

namespace lux::editor::material
{
    struct MaterialPreviewRecipe final
    {
        std::shared_ptr<const world::WorldDescription> world;
        std::shared_ptr<const lux::scene::SceneDescription> scene;
        std::shared_ptr<const simulation::SimulationDescription> simulation;
        asset::AssetId mesh;
        cxx::SharedBytes<> mesh_image, world_volume;
        simulation::ecs::Transform3D camera_pose, light_pose;
        lux::scene::Camera camera;
        rdesc::LightDescription light;
    };
    [[nodiscard]] MaterialPreviewResult<MaterialPreviewRecipe>
    makeMaterialPreviewRecipe(std::span<const render::RenderFeatureRegistration>);
}
''')
# Original includes belong to the pure recipe, except live asset adoption/overlays.
includes=t[:t.index('namespace lux::editor::material')]
includes=includes.replace('#include <lux/engine/editor/material/MaterialPreview.hpp>', '#include <lux/engine/editor/material/MaterialPreviewRecipe.hpp>')
for header in ['RenderSceneState','RenderAssets']:
 includes=includes.replace(f'#include <lux/engine/scene/{header}.hpp>\n','')
includes=includes.replace('#include <lux/engine/process/asset_loading/AssetReadOverlay.hpp>\n','').replace('#include <atomic>\n','')
recipe=includes+'namespace lux::editor::material\n{\n    namespace\n    {\n'+helpers+'    }\n'
recipe+='''    MaterialPreviewResult<MaterialPreviewRecipe>
    makeMaterialPreviewRecipe(std::span<const render::RenderFeatureRegistration> features)
    {
        MaterialPreviewRecipe recipe;
        recipe.mesh = previewIdentity<asset::AssetId>(4);
        auto mesh = sphereImage(recipe.mesh);
        if (!mesh)
            return cxx::unexpected(mesh.error());
        recipe.mesh_image = std::move(*mesh);
        auto world = previewWorld(recipe.world_volume);
        if (!world)
            return cxx::unexpected(world.error());
'''+description+'''
        recipe.world = std::move(*world);
        recipe.scene = std::make_shared<const lux::scene::SceneDescription>(std::move(*scene));
        recipe.simulation = std::make_shared<const lux::simulation::SimulationDescription>(std::move(*rules));
        recipe.camera_pose.translation = {0, 0, 3.5};
        recipe.camera = lux::scene::Camera{lux::scene::PerspectiveProjection{}, true};
        recipe.light_pose.rotation = Eigen::Quaterniond(
            Eigen::AngleAxisd(-0.6, Eigen::Vector3d::UnitX()) * Eigen::AngleAxisd(-0.5, Eigen::Vector3d::UnitY())
        );
        recipe.light.type = lux::rdesc::ELightType::DIRECTIONAL;
        recipe.light.intensity = 3.0F;
        recipe.light.cast_shadow = false;
        return recipe;
    }
}
'''
(b/'src/MaterialPreviewRecipe.cpp').write_text(recipe)
# Rewrite only the moved prefix and construction sections of the original live owner.
t=t[t.index('    struct MaterialPreview::Impl'):]
t='''#include <lux/engine/editor/material/MaterialPreviewRecipe.hpp>
#include <lux/engine/scene/RenderSceneState.hpp>
#include <lux/engine/scene/RenderAssets.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/process/asset_loading/AssetReadOverlay.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <random>

namespace lux::editor::material
{
    namespace ecs = lux::simulation::ecs;
    namespace
    {
        template<class E> auto failed(std::string domain, E error)
        {
            return cxx::unexpected(MaterialPreviewFailure{
                EMaterialPreviewError::PREPARATION, std::move(domain), std::move(error)
            });
        }
        auto busy()
        {
            return cxx::unexpected(MaterialPreviewFailure{EMaterialPreviewError::BUSY, "preview.busy"});
        }
        struct PreparedPreview final
        {
            PreviewAdoptionKey key;
            std::shared_ptr<const CompiledMaterial> compiled;
        };
    }
'''+t
a=t.index('        preview->mesh = previewIdentity');z=t.index('        lux::scene::RenderFeatureSceneBindings bindings',a)
t=t[:a]+'''        auto recipe = makeMaterialPreviewRecipe(environment_.features);
        if (!recipe)
            return cxx::unexpected(recipe.error());
        preview->mesh = recipe->mesh;
        preview->mesh_image = recipe->mesh_image;
        preview->world_volume = recipe->world_volume;
'''+t[z:]
t=t.replace('.setDescription(std::make_shared<const lux::scene::SceneDescription>(std::move(*scene)))','.setDescription(recipe->scene)').replace('.setWorld(std::move(*world))','.setWorld(recipe->world)').replace('.setSimulation(std::make_shared<const lux::simulation::SimulationDescription>(std::move(*rules)))','.setSimulation(recipe->simulation)')
a=t.index('        ecs::Transform3D camera_pose;');z=t.index('        preview->camera = camera;',a)
t=t[:a]+'''        registry.emplace<ecs::Transform3D>(camera, recipe->camera_pose);
        registry.emplace<lux::scene::Camera>(camera, recipe->camera);
        registry.emplace<ecs::Transform3D>(light, recipe->light_pose);
        registry.emplace<ecs::Light3D>(light, recipe->light);
'''+t[z:]
t=t.replace('std::shared_ptr<const CompiledMaterial> displayed, pending;', 'std::optional<PreparedPreview> displayed, pending;')
t=t.replace('const std::uint64_t target_;\n        MaterialCompileInputKey desired_;\n        std::shared_ptr<const CompiledMaterial> prepared_;', 'const uuids::uuid target_;\n        PreviewAdoptionKey desired_;\n        std::optional<PreparedPreview> prepared_;\n        std::optional<VMaterialCompileFailure> compilation_failure_;')
t=t.replace('MaterialPreviewEnvironment environment, std::uint64_t target)', 'MaterialPreviewEnvironment environment, uuids::uuid target)').replace('target_(target)\n        {}','target_(target)\n        { desired_.target = target; }')
t=t.replace('MaterialCompileResult','MaterialPreviewResult').replace('lux::cxx::unexpected(VMaterialCompileFailure{EMaterialCompileRequestError::BUSY})','busy()')
a=t.index('        static std::atomic_uint64_t next{1};');z=t.index('    void MaterialPreview::Impl::update()',a)
t=t[:a]+'''        std::mt19937 random{std::random_device{}()};
        const auto target = uuids::uuid_random_generator{random}();
        impl_ = std::make_unique<Impl>(runtime, std::move(environment), target);
    }
    MaterialPreview::~MaterialPreview() { static_cast<void>(close()); }
    MaterialPreviewResult<PreviewAdoptionKey> MaterialPreview::setDesired(MaterialCompileInputKey input) noexcept
    {
        if (impl_->closing_)
            return cxx::unexpected(MaterialPreviewFailure{EMaterialPreviewError::CLOSED, "preview.closed"});
        if (impl_->desired_.generation && impl_->desired_.input == input)
            return impl_->desired_;
        if (impl_->desired_.generation == UINT64_MAX)
            return cxx::unexpected(MaterialPreviewFailure{EMaterialPreviewError::CAPACITY, "preview.generation"});
        impl_->desired_ = {impl_->target_, impl_->desired_.generation + 1, input, 1, impl_->environment_.scene.version};
        impl_->compilation_failure_.reset();
        return impl_->desired_;
    }
    MaterialPreviewResult<void> MaterialPreview::receive(PreviewAdoptionKey key,
        MaterialCompileResult<std::shared_ptr<const CompiledMaterial>> result, lux::scene::RenderAssetInput assets)
    {
        if (impl_->closing_ || key != impl_->desired_)
            return {}; // Settle accepted stale work without adopting or relabelling it.
        if (!result)
        {
            impl_->compilation_failure_ = std::move(result.error());
            impl_->preview_failure_ = "Compilation failed; previous preview is stale";
            return {};
        }
        if (!*result || (*result)->key() != key.input)
            return cxx::unexpected(MaterialPreviewFailure{EMaterialPreviewError::INVALID_INPUT, "preview.source"});
        impl_->prepared_ = PreparedPreview{key, std::move(*result)};
        impl_->base_ = std::move(assets);
        impl_->compilation_failure_.reset();
        impl_->preview_failure_.clear();
        return {};
    }
'''+t[z:]
t=t.replace('prepared_->artifact','prepared_->compiled->artifact()').replace('prepared_->bytes','prepared_->compiled->bytes()')
t=t.replace('impl_->desired_ = {};','impl_->desired_.input = {};') # keep generation high-water; reset cannot resurrect old request
t=t.replace('        impl_->preview_failure_.clear();\n        return {};\n    }\n    MaterialPreviewResult<void> MaterialPreview::navigate', '        impl_->preview_failure_.clear();\n        impl_->compilation_failure_.reset();\n        return {};\n    }\n    MaterialPreviewResult<void> MaterialPreview::navigate')
t=t.replace('        result.stale =', '        result.compilation_failure = impl_->compilation_failure_;\n        result.stale =')
p.write_text(t)
p=b/'CMakeLists.txt';t=p.read_text().replace('src/MaterialPreview.cpp)','src/MaterialPreview.cpp src/MaterialPreviewRecipe.cpp)');t=t.replace('component_include_directories(material_preview BUILD_TIME_SHARED', 'target_include_directories(material_preview PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude)\ncomponent_include_directories(material_preview BUILD_TIME_SHARED');p.write_text(t)
# Exact provider map for new private recipe implementation.
p=s/'editor/tests/architecture/rules.json';d=json.loads(p.read_text());
def visit(v):
 if isinstance(v,dict):
  for k,x in v.items():
   if isinstance(x,list) and 'editor/activities/material/src/MaterialPreview.cpp' in x:
    x.extend(['editor/activities/material/src/MaterialPreviewRecipe.cpp','editor/activities/material/pinclude/lux/engine/editor/material/MaterialPreviewRecipe.hpp'])
   else:visit(x)
 elif isinstance(v,list):
  for x in v:visit(x)
visit(d);p.write_text(json.dumps(d,indent=2)+'\n')
print('R4 private recipe and per-target adoption owner migrated')
