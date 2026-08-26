#include "type_static_info_types.hpp"
#include <type_static_info_types.type_static_info.hpp>

#include <lux/engine/meta/TypeStaticInfo.hpp>

#include <cassert>
#include <tuple>

int main()
{
    static_assert(lux::meta::HasTypeStaticInfo<StaticInfoProbe>);
    static_assert(std::tuple_size_v<decltype(
        lux::meta::TypeStaticInfo<StaticInfoProbe>::fields
    )> == 2U);
    StaticInfoProbe value{3U, 4.0F, 5U};
    const auto& first = std::get<0>(
        lux::meta::TypeStaticInfo<StaticInfoProbe>::fields
    );
    assert(value.*first.pointer == 3U);
    return 0;
}
