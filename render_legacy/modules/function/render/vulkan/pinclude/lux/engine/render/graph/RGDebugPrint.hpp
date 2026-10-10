#pragma once

#include <iosfwd>
#include <lux/engine/render/graph/RGCompiledGraph.hpp>
#include <lux/engine/function/visibility.h>

namespace lux::render
{
    LUX_FUNCTION_PUBLIC const char* passTypeToString(ERGPassType type);
    LUX_FUNCTION_PUBLIC const char* resourceTypeToString(ERGResourceType t);
    LUX_FUNCTION_PUBLIC void printCompiledGraph(const RGCompiledGraph& compiled, std::ostream& os);
} // namespace lux::render
