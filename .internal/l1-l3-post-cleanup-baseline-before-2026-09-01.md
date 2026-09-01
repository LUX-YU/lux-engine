# L1-L3 Post-Cleanup Before Evidence — 2026-09-01

- Exact baseline: `a39246eeff3542af6345a96f586980e96ba1c367`.
- Branch/remote baseline: `main`, already published to `origin/main`.
- Default evidence at this revision: full build PASS, immediate second build `ninja: no work to do`, CTest 149/149.
- Full Render evidence at this revision: full build PASS, immediate second build no-work, CTest 162/162.
- Real Vulkan: RTX 4070 Ti / NVIDIA 591.86, validation errors 0; SceneSystem vertical path and 10k retained scene PASS.
- Installed consumers: scene-render and dedicated-scene PASS.
- Canonical source evidence: `.internal/l1-l3-final-convergence-evidence-2026-09-01.md`.

Uncommitted user-owned changes preserved for every wave:

```text
.gitignore
engine/domain/world/core/include/lux/engine/world/WorldPartition.hpp
```

The WorldPartition change is formatting-only. Before the World split, its patch is retained, the tracked baseline file
is moved using committed HEAD content, and the formatting patch is reapplied to the new world/partition path without
staging it.
