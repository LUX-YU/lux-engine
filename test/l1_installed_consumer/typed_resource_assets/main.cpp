#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/resource/asset/model/ModelAsset.hpp>

#include <type_traits>

static_assert(std::is_same_v<
    lux::asset::MaterialAsset::data_type,
    lux::rdesc::MaterialDescription
>);
static_assert(std::is_same_v<
    lux::asset::ModelAsset::data_type,
    lux::rdesc::ModelDescription
>);

int main()
{
    return 0;
}
