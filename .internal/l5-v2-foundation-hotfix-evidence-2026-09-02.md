# L5 v2 Foundation Hotfix Qualification Evidence

Date: 2026-09-02

Status: **PASS — Windows foundation continuation gate closed; Android/TSan not claimed**

Qualified source revision: `ec2dce21064df690f4d613860099acf6a04a52ac`

The qualified revision contains every source/build input. This evidence supersedes the closure claim in
`l5-v2-foundation-evidence-2026-09-02.md`, whose tests were run with ignored, untracked source files.

## Implemented repairs

- Removed the repository-wide bare `test` ignore rule and tracked all 14 CMake-referenced tests that had existed only
  in the local working tree. Added two compile-negative TaskScope contract probes.
- Added `cmake/ValidateTrackedSnapshot.cmake`; qualification now rejects dirty worktrees and ignored source/build
  inputs under canonical source roots.
- `EditorApplication::installTool<T>()` now checks application state and Toolset presence before dereference：
  COMPOSING installs, RUNNING returns FROZEN, STOPPING/JOINED returns STOPPING.
- `TaskScope::start()` uses two-phase admission. It never holds its mutex across eager spawn, async-scope stop,
  receiver or user callbacks；close waits for admitted starts to register/fail before starting `on_empty()`.
- TaskScope is compile-time lifetime-only：accepted Senders have only payload-free `set_value()` plus optional
  `set_stopped`; callers must consume value/error before start.
- Graph tests now cover multi-operation rollback and rollback failure poisoning with semantic pre-state comparison.

## Clean tracked snapshot proof

An independent local clone was created from the commit, with build trees outside the source tree.

```text
git rev-parse HEAD
ec2dce21064df690f4d613860099acf6a04a52ac

git status --porcelain
<empty>

git diff --exit-code
<exit 0>

git diff --cached --exit-code
<exit 0>

cmake -DLUX_SOURCE_DIR=<clean-clone> -P cmake/ValidateTrackedSnapshot.cmake
-- Tracked snapshot is clean: ec2dce21064df690f4d613860099acf6a04a52ac
```

`git ls-files --error-unmatch` succeeded for EditorApplication/Context/graph-adapter tests, ExecutionRuntime/TaskScope,
VfsAssetReadEndpoint/AssetVfsView, shared Graph source and resource identity contract tests.

## Windows build and test matrix

Every build used full `all -j 4 -k 0` from a newly configured tree.

```text
Default Developer: 162/162 CTest passed
PLAYER:            162/162 CTest passed
EDITOR:            170/170 CTest passed
TOOLCHAIN:         155/155 CTest passed
Full Render:       178/178 CTest passed
```

Developer, PLAYER, EDITOR, TOOLCHAIN and Full Render each received a second full build；all five reported
`ninja: no work to do`.

The following tests passed 50 consecutive repeat-until-fail runs：AssetVfsView, shared Graph source,
ExecutionRuntime, TaskScope, VfsAssetReadEndpoint, EditorApplication, EditorContext, node graph editor and graph domain
adapters. The TaskScope suite includes inline re-entrant requestStop, start-vs-close admission and stop-vs-start races.

Full Render includes texture/model Vulkan qualification, L1-to-L3 render sync and large-scene performance.

## Installed/public closure

Fresh Developer, Editor and Toolchain prefixes were installed. Fresh relocated consumers configured, built and ran
for all 12 foundation surfaces：AssetVfsView, Process Execution, Process Asset Loading, EditorContext, shared Graph,
Material Graph, Node Graph Editor, Material/Flow Graph adapters, FlowForge model/compiler and Material compiler/cooker.
The FlowForge compiler consumer used the explicit `LUX_FLOWFORGE_LINKER` environment required by its existing AOT
contract.

## Artifact comparison

The clean-clone Material artifacts retain physical source provenance in `WireAssetInfoV2::source_path`, so their raw
hash necessarily changes when the build/clone root changes. Binary comparison proved that this 256-byte field is the
only difference in each Material asset. After normalizing that field, old and clean-clone bytes hash identically：

```text
605ebb80-8566-4f2a-80b6-a153fead1eec  6F0273453DAD61EC22AF91DDEFEBDACE13E609B14449FD3D33BDF9D01B0D7CC9
ccf44e8f-2220-459f-81a8-566d5c6d4b26  8DA9E0ED0A32D9B6866F5EB75C575AD0BAB2BB4FC6D2C1DEC8568FAB5E23B8FB
ce5f070e-f086-49da-b516-bf6febd75a0f  3932F7A386AD99E6698AB4F07E8158F6D8B35B4BCD0FFD2968E6AFD074A7F874
```

The Pak raw hash also changes because it contains those provenance-bearing assets and their content hashes；the
tracked ModelPak roundtrip/typed decode tests passed. No Material payload/compiler semantic drift was observed.

## Log checksums

```text
Developer full   CEB24650291E86542A6F60FF4F8508AAD2F3D3778C1820A6FE2BE06415C1D008
Developer stress CB6D4734C0DBCF08E06C464795EACBF0778FC92F6F5743BF5C453A3DE5D44896
EDITOR full      34A58B4C314448199D73A1168C6CF564ED219C780F31457B485D9A9A99D6DE2A
EDITOR stress    3292C318326B137B5B7B6A6F6E6579BCBC2FC8233BCAFE79703F840C6C352A66
PLAYER full      DB14BAAF0082343E45E1DE81CD673D7EE6CD75D70266DBB165F1787DB5400989
TOOLCHAIN full   135F3900719B5F47CACDB46A9061AF0E61552DC1CC63A5895B18D33E8EE4D555
Full Render full 37461026EB1705E54DA0E18952A8057F073CAD472547074EE0B3545F211C536A
```

## Explicitly unclaimed

- Per user decision, Android configure/build and Linux ThreadSanitizer were not run. This evidence closes the selected
  Windows continuation gate only.
- C/D/E/S/H/I/P remain outside this hotfix. No GenericHost, AssetIndex, ProcessSender, persistence format or other
  out-of-wave abstraction was introduced.
