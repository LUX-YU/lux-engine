#pragma once

#include <lux/cxx/reflection/runtime/Marker.hpp>

#define LUX_RENDER_DATA(...) LUX_META(luxdata::schema, __VA_ARGS__)
#define LUX_DATA_VALUE() LUX_META(luxdata::value)
#define LUX_GPU_FIELD(...) LUX_META(luxdata::gpu, __VA_ARGS__)
