#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct LUX_PASS_PARAMS() BadLayout
{
    unsigned int a : 3;
    unsigned int b : 5;
};
