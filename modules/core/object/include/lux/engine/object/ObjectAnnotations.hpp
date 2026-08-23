#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>

#define LUX_OBJECT(...) LUX_CLASS(object=true __VA_OPT__(,) __VA_ARGS__)
#define LUX_OBJECT_SIGNAL(name) LUX_MEMBER(signal=true, signal_name=name)
