#pragma once
/**
 * @file PreviewWorldCommon.hpp
 * @brief 编辑器两个私有预览 SceneRuntime(ThumbnailService 缩略图世界 +
 *        MaterialPreviewHost 活材质预览世界)共用的装配小件。
 *
 * 两个宿主的世界形状一致:手工 preview pack(Transform / Camera / Animation +
 * Mesh / CameraUpload / DirectionalLight 渲染子系统)+ 常驻 key light + 一台
 * 自带离屏 target 的取景相机。差异只剩「相机 aspect 谁说了算」(缩略图钉 1:1,
 * 活预览跟随面板尺寸)与各自的业务状态机 —— 所以共用的只有这四个装配函数,
 * 状态机各归各家。编辑器私有(src/ 下,不进公共 include/)。
 */

#include <lux/engine/runtime/extensions/SceneContributions.hpp>
#include <lux/engine/ecs/Registry.hpp>              // entity_id
#include <lux/engine/function/render/client/core/FeatureHandle.hpp>   // RenderTargetId
#include <lux/engine/math/Extent.hpp>
#include <lux/engine/resource/asset/Asset.hpp>                 // asset_id_t
#include <lux/engine/scene/SceneDescription.hpp>

#include <Eigen/Core>

#include <string_view>

namespace lux::ecs { class World; }
namespace lux::asset { class AssetManager; }

namespace lux::editor
{
    inline constexpr std::string_view kPreviewWorldContributionName =
        "org.lux.editor.preview3d";

    [[nodiscard]] constexpr lux::scene::SceneFeatureIdView
    previewWorldContributionId() noexcept
    {
        return lux::scene::sceneFeatureId(
            kPreviewWorldContributionName);
    }

    /// 手工 preview pack —— 故意**不用 addScene3D**:预览世界只要「网格实例画得
    /// 出来」这一件事。骨骼/天空盒/流送/剔除都不装 —— 少装的每一项都是每帧的
    /// 空查询与一次 feature attach。AnimationSystem 仍要装:SceneRuntime 无条件
    /// 安装的 SkeletalAnimationResolver 声明了「先于 AnimationSystem」,不装的话
    /// 那条约束悬空,Schedule::compile 每次装配都会 warn 一行。
    [[nodiscard]] lux::runtime::SceneContributionDescriptor
    makePreviewWorldContribution();

    /// Build the transient LXSC value for one private preview world. Registering
    /// the descriptor in a catalog is not activation: the manifest must select
    /// it so SceneRuntime installs the mesh resolver and extraction subsystem.
    [[nodiscard]] lux::scene::SceneDescription
    makePreviewSceneDescription(std::string_view scene_name);

    /// Register the deterministic private preview SceneAsset in the existing
    /// process AssetManager and return its id. Repeated registration of the
    /// same preview id reuses the already-present asset.
    [[nodiscard]] lux::asset::asset_id_t registerPreviewSceneAsset(
        lux::asset::AssetManager& assets,
        std::string_view scene_name);

    /// 常驻 key light(方向/暖白/强度/投影与旧手写预览场景的常驻灯同参)。
    lux::ecs::Entity createPreviewKeyLight(lux::ecs::World& world);

    /// 取景相机实体：Transform3D + ResolvedTransform3D + Camera3D（fov 45°，
    /// render-client canonical Y-down projection)+ ViewPresentComponent{target, 0, extent}。
    /// @p auto_aspect false = 钉 1:1(缩略图方图);true = 随 ViewPresent extent
    /// 派生(活预览面板改尺寸时 patch extent 即可)。
    /// 调用方装配末尾要自己 `runtime->settleViewCreation()`(第一帧提交之前
    /// view 必须在位,否则 target 上没有任何层)。
    lux::ecs::Entity createPreviewCamera(lux::ecs::World&            world,
                                             lux::render::RenderTargetId target,
                                             lux::math::Extent2u         extent,
                                             bool                        auto_aspect);

    /// 把相机实体摆到 @p eye、看向 @p center:写 Transform3D 位姿(相机沿局部
    /// -Z 看,Camera3DSystem 的约定 —— 构造 col2 = -forward 的右手正交基)。
    /// 前置:eye→center 方向不与世界 up(+Y)平行(两处调用方的取景方向都带
    /// 水平分量,叉积无退化)。
    void aimPreviewCamera(lux::ecs::World&       world,
                          lux::ecs::Entity   camera,
                          const Eigen::Vector3f& eye,
                          const Eigen::Vector3f& center);

    /// 编译期字面量 UUID → asset_id_t。false = 字面量本身写坏了(程序员错误)。
    bool parseBuiltinId(const char* s, lux::asset::asset_id_t& out) noexcept;

} // namespace lux::editor
