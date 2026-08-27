#pragma once
//
// StateLayout.hpp — the instance-state block layout of a FlowGraph.
//
// Graph variables are NOT baked into the compiled script: every generated
// function takes a leading `state` pointer and reads/writes its variables
// at fixed byte offsets inside that host-owned block (the UE-Blueprint
// object-property model). Externalizing the state is what lets one compiled
// graph attach to many objects (each with its own block), lets a hot reload
// swap the code while keeping the state, and gives the AOT/plugin path its
// manifest entry (name / type / offset / default per variable).
//
// computeStateLayout is the single source of truth: the MLIR builder derives
// its GEP offsets from it, JIT hosts allocate and initialize blocks from it,
// and the cook step serializes it into the script manifest.
//
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <lux/engine/function/visibility.h>

namespace lux::meta { struct RefType; }

namespace lux::flowforge
{
    class FlowGraph;

    struct StateFieldLayout
    {
        uint64_t                  var_id = 0;
        std::string               name;
        const lux::meta::RefType* type   = nullptr;
        uint32_t                  offset = 0;   ///< byte offset inside the block
    };

    struct StateLayout
    {
        std::vector<StateFieldLayout> fields;    ///< ordered by var_id
        uint32_t                      size  = 0; ///< total block bytes (0 = stateless graph)
        uint32_t                      align = 1; ///< required block alignment
        uint64_t                      hash  = 0; ///< stable layout identity (hot-reload compare)
        std::vector<std::byte>        defaults;  ///< `size` bytes: default values at offsets

        const StateFieldLayout* find(uint64_t var_id) const
        {
            for (const auto& f : fields)
                if (f.var_id == var_id) return &f;
            return nullptr;
        }
    };

    /// Computes the block layout over ALL graph variables (ordered by their
    /// stable id, natural scalar alignment, block size rounded up to the
    /// block alignment). On an invalid variable (missing type, non-scalar,
    /// missing or type-mismatched default value) writes a diagnostic to
    /// error_out and returns an EMPTY layout.
    StateLayout computeStateLayout(
        const FlowGraph& graph, std::string* error_out = nullptr);
}
