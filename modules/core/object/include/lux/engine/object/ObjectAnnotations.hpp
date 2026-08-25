#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>

#define LUX_OBJECT(...) \
    LUX_TYPE_INFO(runtime, object = true __VA_OPT__(, ) __VA_ARGS__)
