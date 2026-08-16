#pragma once

#include <lux/engine/function/render/graph/RGLogicalTypes.hpp>
#include <lux/engine/function/visibility.h>

namespace lux::render
{
    class LUX_FUNCTION_PUBLIC DependencyAnalyzer final
    {
    public:
        [[nodiscard]] static RGDependencyInfo analyze(
            RGLogicalGraphView graph
        );
    };
} // namespace lux::render
