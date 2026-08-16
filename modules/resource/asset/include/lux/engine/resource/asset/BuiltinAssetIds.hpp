#pragma once
/**
 * @file BuiltinAssetIds.hpp
 * @brief 引擎级内置资产的**冻结** UUID —— ecs 与 editor 的最低公共层。
 *
 * 为什么在 asset 模块而不是 EditorBuiltins.hpp:消费者不止编辑器 ——
 * ecs 层的资源解析器要在「资产确认不存在」时把实体换装到 M_Missing
 * 醒目材质(删除扩散裁决七:实体变色,不消失),而 ecs 够不着 editor 库。
 * EditorBuiltins.hpp 的同名常量转引这里,双份字面量禁止出现。
 *
 * 与 AssetHeaderProbe.hpp 的 World magic 同一条纪律:一旦有资产/世界
 * 引用过这些 id,**永不改动**(改动 = 既有内容全部悬空)。
 */

#include <lux/engine/resource/asset/AssetId.hpp>

namespace lux::asset
{
    inline constexpr const char* kBuiltinCubeMeshIdStr =
        "00000000-0000-4000-8000-cccccccccccc";
    inline constexpr const char* kBuiltinPlaneMeshIdStr =
        "00000000-0000-4000-8000-ffffffffffff";
    inline constexpr const char* kBuiltinSphereMeshIdStr =
        "00000000-0000-4000-8000-eeeeeeeeeeee";
    inline constexpr const char* kBuiltinWhitePbrMaterialIdStr =
        "00000000-0000-4000-8000-aaaaaaaaaaaa";
    inline constexpr const char* kBuiltinPreviewGreyMaterialIdStr =
        "00000000-0000-4000-8000-bbbbbbbbbbbb";

    /// M_Missing —— 「资产被删/引用悬空」的醒目兜底材质(magenta 系)。
    /// 走 engine_content 烘焙路(lux_content_baker 落盘 + EditorBuiltins 进程
    /// 内注册),编辑器与 player 都解析得到 —— 纯进程注册的 PreviewGrey 在
    /// player 里不存在,这正是它不能照抄的原因。
    inline constexpr const char* kBuiltinMissingMaterialIdStr =
        "00000000-0000-4000-8000-dddddddddddd";

    inline constexpr int kBuiltinEmissiveCount = 8;
    inline constexpr const char* kBuiltinEmissiveIdStrs[
        kBuiltinEmissiveCount] = {
        "00000000-0000-4000-8001-000000000001",
        "00000000-0000-4000-8001-000000000002",
        "00000000-0000-4000-8001-000000000003",
        "00000000-0000-4000-8001-000000000004",
        "00000000-0000-4000-8001-000000000005",
        "00000000-0000-4000-8001-000000000006",
        "00000000-0000-4000-8001-000000000007",
        "00000000-0000-4000-8001-000000000008",
    };
    inline constexpr const char* kBuiltinWhitePbrInstanceIdStr =
        "00000000-0000-4000-8002-aaaaaaaaaaaa";
    inline constexpr const char* kBuiltinEmissiveInstanceIdStrs[
        kBuiltinEmissiveCount] = {
        "00000000-0000-4000-8002-000000000001",
        "00000000-0000-4000-8002-000000000002",
        "00000000-0000-4000-8002-000000000003",
        "00000000-0000-4000-8002-000000000004",
        "00000000-0000-4000-8002-000000000005",
        "00000000-0000-4000-8002-000000000006",
        "00000000-0000-4000-8002-000000000007",
        "00000000-0000-4000-8002-000000000008",
    };

    inline constexpr float kBuiltinEmissiveColors[
        kBuiltinEmissiveCount][3] = {
        {1.00f, 0.90f, 0.70f},
        {1.00f, 0.15f, 0.10f},
        {0.20f, 1.00f, 0.25f},
        {0.20f, 0.40f, 1.00f},
        {1.00f, 0.55f, 0.10f},
        {0.10f, 0.90f, 1.00f},
        {1.00f, 0.20f, 0.80f},
        {0.60f, 0.30f, 1.00f},
    };

    /// 运行期解析好的 id(函数内静态,首个调用者付一次 from_string)。
    [[nodiscard]] inline const asset_id_t& builtinMissingMaterialId()
    {
        static const asset_id_t id =
            asset_id_t::from_string(kBuiltinMissingMaterialIdStr).value();
        return id;
    }
} // namespace lux::asset
