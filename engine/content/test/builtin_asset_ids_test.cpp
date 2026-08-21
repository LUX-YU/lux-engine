#include <lux/engine/content/BuiltinAssetIds.hpp>

#include <array>
#include <cassert>
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

    std::unordered_set<std::string_view> unique;
    for (const auto text : ids)
    {
        const auto parsed = lux::asset::asset_id_t::from_string(text);
        assert(parsed.has_value());
        assert(!parsed->is_nil());
        assert(unique.insert(text).second);
    }
    assert(
        builtinMissingMaterialId()
        == lux::asset::asset_id_t::from_string(kBuiltinMissingMaterialIdStr).value()
    );
    return 0;
}
