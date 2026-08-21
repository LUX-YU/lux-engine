#include <lux/engine/content/BuiltinAssetIds.hpp>

int main()
{
    return lux::engine::content::builtinMissingMaterialId().is_nil() ? 1 : 0;
}
