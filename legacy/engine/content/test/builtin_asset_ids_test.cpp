#include <lux/engine/content/BuiltinAssetIds.hpp>

#include <array>
#include <cstddef>
#include <iostream>
#include <string_view>
#include <unordered_set>

int main()
{
    using namespace lux::engine::content;

    const std::array ids{
        std::string_view{kBuiltinCubeMeshIdStr},
        std::string_view{kBuiltinPlaneMeshIdStr},
        std::string_view{kBuiltinSphereMeshIdStr},
        std::string_view{kBuiltinWhitePbrMaterialIdStr},
        std::string_view{kBuiltinPreviewGreyMaterialIdStr},
        std::string_view{kBuiltinMissingMaterialIdStr},
        std::string_view{kBuiltinWhitePbrInstanceIdStr},
        std::string_view{kBuiltinEmissiveIdStrs[0]},
        std::string_view{kBuiltinEmissiveIdStrs[1]},
        std::string_view{kBuiltinEmissiveIdStrs[2]},
        std::string_view{kBuiltinEmissiveIdStrs[3]},
        std::string_view{kBuiltinEmissiveIdStrs[4]},
        std::string_view{kBuiltinEmissiveIdStrs[5]},
        std::string_view{kBuiltinEmissiveIdStrs[6]},
        std::string_view{kBuiltinEmissiveIdStrs[7]},
        std::string_view{kBuiltinEmissiveInstanceIdStrs[0]},
        std::string_view{kBuiltinEmissiveInstanceIdStrs[1]},
        std::string_view{kBuiltinEmissiveInstanceIdStrs[2]},
        std::string_view{kBuiltinEmissiveInstanceIdStrs[3]},
        std::string_view{kBuiltinEmissiveInstanceIdStrs[4]},
        std::string_view{kBuiltinEmissiveInstanceIdStrs[5]},
        std::string_view{kBuiltinEmissiveInstanceIdStrs[6]},
        std::string_view{kBuiltinEmissiveInstanceIdStrs[7]},
    };

    constexpr std::array<std::string_view, 23> expected_ids{
        "00000000-0000-4000-8000-cccccccccccc",
        "00000000-0000-4000-8000-ffffffffffff",
        "00000000-0000-4000-8000-eeeeeeeeeeee",
        "00000000-0000-4000-8000-aaaaaaaaaaaa",
        "00000000-0000-4000-8000-bbbbbbbbbbbb",
        "00000000-0000-4000-8000-dddddddddddd",
        "00000000-0000-4000-8002-aaaaaaaaaaaa",
        "00000000-0000-4000-8001-000000000001",
        "00000000-0000-4000-8001-000000000002",
        "00000000-0000-4000-8001-000000000003",
        "00000000-0000-4000-8001-000000000004",
        "00000000-0000-4000-8001-000000000005",
        "00000000-0000-4000-8001-000000000006",
        "00000000-0000-4000-8001-000000000007",
        "00000000-0000-4000-8001-000000000008",
        "00000000-0000-4000-8002-000000000001",
        "00000000-0000-4000-8002-000000000002",
        "00000000-0000-4000-8002-000000000003",
        "00000000-0000-4000-8002-000000000004",
        "00000000-0000-4000-8002-000000000005",
        "00000000-0000-4000-8002-000000000006",
        "00000000-0000-4000-8002-000000000007",
        "00000000-0000-4000-8002-000000000008",
    };
    constexpr float expected_colors[kBuiltinEmissiveCount][3]{
        {1.00f, 0.90f, 0.70f},
        {1.00f, 0.15f, 0.10f},
        {0.20f, 1.00f, 0.25f},
        {0.20f, 0.40f, 1.00f},
        {1.00f, 0.55f, 0.10f},
        {0.10f, 0.90f, 1.00f},
        {1.00f, 0.20f, 0.80f},
        {0.60f, 0.30f, 1.00f},
    };

    std::unordered_set<std::string_view> unique;
    bool success = ids.size() == expected_ids.size();
    for (std::size_t index = 0u; index < ids.size(); ++index)
    {
        const auto text = ids[index];
        const auto parsed = lux::asset::asset_id_t::from_string(text);
        success = success && text == expected_ids[index] && parsed &&
            !parsed->is_nil() && unique.insert(text).second;
    }
    for (int color = 0; color < kBuiltinEmissiveCount; ++color)
        for (int lane = 0; lane < 3; ++lane)
            success = success &&
                kBuiltinEmissiveColors[color][lane] ==
                    expected_colors[color][lane];
    const auto expected_missing = lux::asset::asset_id_t::from_string(
        "00000000-0000-4000-8000-dddddddddddd"
    );
    success = success && expected_missing &&
        builtinMissingMaterialId() == *expected_missing;
    if (!success)
        std::cerr << "frozen Engine Content identity or palette changed\n";
    return success ? 0 : 1;
}
