#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>

struct LUX_OP(lane=program, kind=stream, name="test.unsafe.pointer.v1") UnsafePointer final
{
    const char* borrowed{};
};
