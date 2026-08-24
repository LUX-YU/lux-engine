#pragma once
/**
 * @file BuiltinAssetIds.hpp
 * @brief Lux Engine 默认内容的冻结 UUID 与色板。
 *
 * 这些身份由 builtin baker、Runtime 和 Editor 共用，但不是通用
 * Resource Asset SDK 的语义。ECS 不包含本头；Runtime 在装配点将
 * M_Missing ID 显式注入 Residency。
 *
 * 与 AssetHeaderProbe.hpp 的 World magic 同一条纪律:一旦有资产/世界
 * 引用过这些 id,**永不改动**(改动 = 既有内容全部悬空)。
 */

#include <lux/engine/resource/asset/AssetId.hpp>
#include <lux/engine/content/visibility.h>

namespace lux::engine::content
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
    inline constexpr const char* kBuiltinEmissiveInstanceIdStrs[kBuiltinEmissiveCount] = {
        "00000000-0000-4000-8002-000000000001",
        "00000000-0000-4000-8002-000000000002",
        "00000000-0000-4000-8002-000000000003",
        "00000000-0000-4000-8002-000000000004",
        "00000000-0000-4000-8002-000000000005",
        "00000000-0000-4000-8002-000000000006",
        "00000000-0000-4000-8002-000000000007",
        "00000000-0000-4000-8002-000000000008",
    };

    inline constexpr float kBuiltinEmissiveColors[kBuiltinEmissiveCount][3] = {
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
    [[nodiscard]] LUX_ENGINE_CONTENT_PUBLIC
    const lux::asset::asset_id_t& builtinMissingMaterialId();
} // namespace lux::engine::content
