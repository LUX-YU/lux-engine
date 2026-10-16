# v12 N0 supplement — accepted complete work breakdown

The user accepted 38 work packages on 2026-10-16. `work-packages.json` records their
individual scope; `requirements.json` maps 16 decisions, 28 review findings, 39 historical
issues and three protected contracts without colliding D identifiers. `member-dispositions.json`
preserves all 229 source audit groups and assigns review responsibility; its pending statuses
are deliberately not implementation or verification claims. `deletion-map.json` covers all 34
rows of contract section 15.3. These are new planning/verification records, not changed past evidence.

## Source correction

The original N0 inventory included Editor workspace LayoutPlan and MaterialGraphEdit because
of a broad filename search. They are unrelated to GPU layout allocation and are excluded in
`algorithm-sources-corrected.json`. The original inventory and N0 report remain unmodified.
The actual render sources are the frozen RenderGraphCompiler.LayoutAllocator, rendering
LayoutPlan and SpirvPatcher, plus the current lglsl Pass emitter and rdesc contracts.
Neither Editor Legacy nor Render Legacy is compiled or modified by this migration.

## Revised qualification boundary

The user's explicit decision allows only limited include/namespace/target migration in upper
Engine/Editor consumers. Comprehensive builds/tests cover Render, its direct dependencies,
necessary build tools and independent installed consumers. Upper product compilation/testing
is NOT_RUN unless a recorded attempt actually ran. Three existing Editor dependencies available
only in frozen Legacy (RenderContext, EngineRendering, PluginRendering) remain BLOCKED.
Their absence no longer blocks the Render gates. No Legacy fallback, placeholder target or
product-closure PASS is authorized. Prior attempted consumer compilation logs are retained.

## Gate ordering

Finish N1 before dependent production changes. Implement N2/N3 as one converging seam:
typed ConfigValue is an N2 prerequisite, and deletion of GPU registration classification is
not qualified until the generic Reply/Pending path and real Native completion regressions pass.
N4 retirement ownership must pass before N5 rebinding and N6 resource consumers. Preserve
the fixed wait policy until the independently measured N7 scheduling commit.

## Current facts

Remote remains `78bcd220815f845466d85d31f994901e4db224ba`. Isolated implementation HEAD is
`066d9f400fcf24040bea482c1b77e596c453c313`; N1 production changes are not yet committed.
Historical performance is PARTIAL. This supplement adds no functional PASS.
