#version 450
// =========================================================================
//  highlight_mask.frag — writes 1.0 into the R8 highlight mask for every
//  covered fragment of a highlighted instance. The highlight post-process
//  reads this mask, blurs it, and composites the outer halo over SceneColor.
//  (Step 1 draws ALL visible instances; Step 3 adds the HIGHLIGHT-flag filter.)
// =========================================================================
layout(location = 0) flat in  uint  vFlags;
layout(location = 0)      out float outMask;

// HighlightOperation.hpp kInstanceFlagHighlight (1u << 3). The cull/draw covers ALL
// visible instances; only HIGHLIGHTED ones contribute to the mask — the rest are
// discarded here (a dedicated HIGHLIGHT-only cull is a later perf increment).
const uint INSTANCE_FLAG_HIGHLIGHT = 8u;

void main()
{
    if ((vFlags & INSTANCE_FLAG_HIGHLIGHT) == 0u)
        discard;
    outMask = 1.0;
}
