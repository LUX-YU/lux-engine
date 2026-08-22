#include <lux/engine/editor/scene/EditorScene.hpp>
#include <lux/engine/ecs/navigation/NavigationQueryService.hpp>
#include "app/EditorAsyncService.hpp"
#include "scene/WorldPersistenceSchemaClosure.hpp"
#include <lux/engine/editor/scene/WorldActorEcsAdapter.hpp>
#include <lux/engine/authoring/world/WorldSourceCodec.hpp>
#include <lux/engine/authoring/world/WorldTerrainAuthoring.hpp>
#include <lux/engine/runtime/frame/MainCloseDriver.hpp>
#include <lux/engine/runtime/scene/script/SceneScriptRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>
#include <lux/engine/editor/scene/systems/CameraSceneSystem.hpp>     // ONE camera system; per-kind navigator (C9)
#include <lux/engine/editor/scene/systems/SelectionSceneSystem.hpp>  // registered in bringUp
#include <lux/engine/editor/app/EditorActions.hpp>
#include <lux/engine/editor/app/Selection.hpp>
#include <lux/engine/editor/app/LuxEditor.hpp>     // EditorRenderInfra
#include <lux/engine/runtime/render/scene/StandardFeaturePlan.hpp>   // standardDesktopProfile
#include <lux/engine/runtime/render/scene/ResidencyAssembly.hpp>
#include <lux/engine/editor/scene/EditorTransient.hpp>

#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/AssetCodecCatalog.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/scene/SceneAsset.hpp>
#include <lux/engine/scene/SceneAssetSerDeser.hpp>
#include <lux/cxx/algorithm/Sha256.hpp>
#include <lux/engine/ecs/render/components/3d/AnimatorComponent.hpp>
#include <lux/engine/ecs/systems/HierarchicalTransformSystem.hpp> // setParent (per the write contract)
#include <lux/engine/ecs/script/systems/ScriptSystem.hpp>                 // dispatchTo (play wiring)
#include <lux/engine/ecs/script/backends/LuaScriptBackend.hpp>
#include <lux/engine/ecs/script/backends/NativeModuleScriptBackend.hpp>

#include <lux/cxx/core/Format.hpp>   // lux::format — spawnModel entity naming
#include <lux/engine/math/Extent.hpp>
#include <lux/engine/math/Intersection.hpp>
#include <lux/engine/math/Picking.hpp>
#include <lux/engine/math/Ray.hpp>
#include <lux/engine/math/RelativePosition.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/render/components/3d/Camera3DComponent.hpp>
#include <lux/engine/ecs/render/components/3d/Camera3DCacheComponent.hpp>
#include <lux/engine/ecs/render/components/3d/DirectionalLightComponent.hpp>
#include <lux/engine/ecs/render/components/3d/SceneSettingsComponent.hpp>
#include <lux/engine/ecs/render/components/3d/Grid3DComponent.hpp>
#include <lux/engine/ecs/render/components/3d/MeshComponent.hpp>
#include <lux/engine/ecs/render/components/3d/SkeletalMeshComponent.hpp>
#include <lux/engine/ecs/components/NameComponent.hpp>
#include <lux/engine/ecs/render/components/PrimaryCameraTag.hpp>
#include <lux/engine/log/Log.hpp>
#include <lux/engine/ecs/render/components/3d/SkyboxComponent.hpp>
#include <lux/engine/ecs/components/Transform3DComponent.hpp>
#include <lux/engine/ecs/components/Transform2DComponent.hpp>
#include <lux/engine/ecs/render/components/2d/Camera2DComponent.hpp>
#include <lux/engine/ecs/render/components/2d/Grid2DComponent.hpp>
#include <lux/engine/ecs/render/components/2d/Camera2DCacheComponent.hpp>
#include <lux/engine/ecs/render/components/2d/Image2DComponent.hpp>
#include <lux/engine/ecs/render/systems/2d/Camera2DSystem.hpp>   // screenToWorld
#include <lux/engine/ecs/components/ResolvedTransform2DComponent.hpp>
#include <lux/engine/ecs/render/components/RenderViewBindingComponent.hpp>
#include <lux/engine/ecs/components/ResolvedTransform3DComponent.hpp>
#include <lux/engine/ecs/SpatialTransformMath.hpp>
#include <lux/engine/ecs/components/PersistentEntityIdComponent.hpp>
#include <lux/engine/ecs/PersistentEntityIndex.hpp>
#include <lux/engine/ecs/HierarchyIndex.hpp>   // hierarchyRoot (pick -> whole object)

#include <iostream>   // teardown-drain overflow diagnostic
#include <lux/engine/ecs/script/systems/ScriptSystem.hpp>     // play-scoped ScriptSystem + ScriptContext
#include <lux/engine/function/render/client/RenderFrameSession.hpp>
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/resources/lighting/LightDescriptor.hpp>
#include <lux/engine/function/render/client/genops/DeferredGBufferOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/DeferredLightingOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/Grid3DOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MaterialOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/TonemapOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshShadowOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ShadowMapOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/SkyboxOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ViewCameraOperation.ops.hpp>  // seed-push ViewCameraProxy
#include <lux/engine/ui/ImGuiCommConfig.hpp>
#include <lux/engine/ui/UIRenderFrameSession.hpp>

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lux::editor
{
    [[nodiscard]] static lux::ecs::PersistentEntityIndex*
    scenePersistentEntities(lux::runtime::SceneRuntime* runtime) noexcept
    {
        return runtime
            ? runtime->services().get<lux::ecs::PersistentEntityIndex>()
            : nullptr;
    }

    bool EditorScene::isPlanar2D() const noexcept
    {
        return world_source_ && !world_source_->spaces.empty() &&
            world_source_->spaces.front().topology ==
                lux::authoring::EPartitionTopology::PLANAR_XY;
    }

#include "EditorScene.WorldEditing.inl"
#include "EditorScene.WorldDomains.inl"
#include "EditorScene.Lifecycle.inl"
#include "EditorScene.PlayCook.inl"
#include "EditorScene.Interaction.inl"

} // namespace lux::editor
