#include <lux/engine/render/graph/RenderGraph.hpp>

namespace lux::render::compiled
{
    bool RenderGraph::compatible(const TargetDesc& target) const noexcept
    {
        if (target.outputContract() != output_description_)
        {
            return false;
        }
        const bool fixed_extent_mismatch = fixed_output_extent_[0] != 0 && (target.width != fixed_output_extent_[0] ||
                                                                            target.height != fixed_output_extent_[1]);
        if (fixed_extent_mismatch)
        {
            return false;
        }
        // Instance creation qualifies resolved extents and copy pitches against actual backing capacities.
        // Resize can retain this immutable plan while preparing a complete candidate before publication.
        return true;
    }

} // namespace lux::render::compiled
