#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
namespace consumer
{
    struct LUX_TYPE_INFO(static) Nested final
    {
        double LUX_MEMBER(display_name = NestedAmount) amount{2};
    };
}
