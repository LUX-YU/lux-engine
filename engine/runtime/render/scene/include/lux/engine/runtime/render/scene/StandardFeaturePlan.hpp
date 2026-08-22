#pragma once
/**
 * @file StandardFeaturePlan.hpp
 * @brief The STANDARD render-feature registration + per-scene attach plan
 *        (lux::runtime).
 *
 * Moved out of the editor (was registerEditorRenderFeaturePlan): this is the
 * engine's standard pipeline preset — lights/shadows/deferred stack/skybox/
 * tonemap/2D canvas… — and a game host replays exactly the same plan on ITS
 * render thread. The editor calls this and then adds its ImGui feature on
 * top; nothing in here is editor-specific (LineList carries any host's debug
 * lines; Grid2D/3D render only when a grid entity exists in the scene).
 *
 * MUST run on the render server's thread before scenes are created
 * (addFeatureFactory is same-thread). Self-hosted harnesses (thumbnail GPU
 * test) call it on their own server thread for the same reason.
 */

#include <lux/engine/runtime/render/scene/visibility.h>
#include <lux/engine/function/render/client/FeatureCatalog.hpp>   // FeatureCatalog / FeatureAttach

#include <vector>

namespace lux::render { class GeneralRenderServer; }

namespace lux::runtime
{
    /// 一个渲染 profile —— **产品/平台维度**的管线选择(装配归属 ADR 裁决一
    /// 的第一层),纯数据:
    ///   · pass_roots:没有 ECS 渲染节点驱动的纯渲染图 pass(管线的形状,
    ///     例如延迟栈 + 后处理)。它与节点的 `requiredFeatures` 并集拼成
    ///     attach 解析器的根。
    ///   · name:配置变体的匹配键 —— 同名 feature 的 FeatureAttach 行里
    ///     `profile == name` 的胜出(缩略图预览的小阴影图集就是这样一行)。
    ///
    /// ⚠️ 与 `EFeatureLevel` / `level_profiles`(设备能力协商,attach 期按实机
    /// 定档)是**两根轴,组合而非合并**:profile 决定产品想装什么,档位协商
    /// 决定这台设备装不装得上。
    ///
    /// 「pass 根」由 render-scene 产品组合显式拥有。这里只列选择该 pipeline
    /// 就必须存在、且与 World content 无关的 roots；具体 System/Stage 的能力
    /// 由其冷装配代码追加。
    struct RenderProfile
    {
        std::string_view                  name{};        ///< FeatureAttach::profile 匹配键
        std::span<const std::string_view> pass_roots{};  ///< 指向静态存储
    };

    /// 标准桌面档:延迟管线 + 阴影 + 后处理 + 遮挡剔除(即此前 3D 包声明的
    /// 那 7 条纯 pass)。桌面编辑器与 lux_player 都用它。
    LUX_RUNTIME_RENDER_SCENE_PUBLIC const RenderProfile& standardDesktopProfile() noexcept;

    /// 预览档:前向管线 + 阴影,无延迟栈/后处理 —— 缩略图与材质预览这类
    /// 「一个物体 + 一盏灯 + 离屏小图」的场景。name = "preview" 是配置变体
    /// 匹配键:ShadowMap 的 "preview" 小图集配置行(见 registerStandardRenderFeatures)
    /// 因此在 `settleRenderCapabilities` 时胜过标准大图集。
    LUX_RUNTIME_RENDER_SCENE_PUBLIC const RenderProfile& previewProfile() noexcept;

    /// Register every standard feature TYPE (name → type-id + dynamic op-ids)
    /// on @p server and build the per-scene ATTACH plan —— 纯数据条目
    /// (name/profile/type_id/config 字节),发出走 addFeatureRaw。
    /// @p catalog / @p plan are overwritten.
    LUX_RUNTIME_RENDER_SCENE_PUBLIC void registerStandardRenderFeatures(
        lux::render::GeneralRenderServer&        server,
        lux::render::FeatureCatalog&             catalog,
        std::vector<lux::render::FeatureAttach>& plan);

} // namespace lux::runtime
