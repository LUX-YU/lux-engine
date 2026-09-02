# L5 v2 Foundation B/V1/V2/A/F/G Qualification Evidence

Date: 2026-09-02

Status: **Foundation tranche complete; implementation stopped before C/D/E/S/H/I/P**

Starting revision: `75dffbf48b1f8b172299ce40d48025db41e867ab`

## Implemented MUST items

- Active architecture numbering ends at L5 Editor. Product remains a build-closure dimension; `HOST` is no longer an
  active classifier and `PLAYER` is a runtime-clean qualification profile only.
- L2 owns bounded CPU/Main/optional Blocking schedulers, Timer, PortSender and structured TaskScope. Blocking was
  introduced only in V2, after the Wave B CPU/Main/TaskScope contract was closed.
- `AssetVfs` is a non-copyable mutable control plane. Copyable `AssetVfsView` reads an atomically published immutable
  mount snapshot and retains providers for the complete read call.
- `VfsAssetReadEndpoint` owns bounded read admission and structured operation lifetime; synchronous provider open runs
  on the shared BlockingScheduler rather than the submitter/game/UI thread.
- L5 `EditorApplication` owns Runtime, root TaskScope, mutable VFS, AssetRead endpoint, Toolset, Selection, UISession and
  SceneMetaManager. It directly produces the headless `lux_editor` bootstrap and performs ordered shutdown.
- `EditorContext` is non-owning and exposes only Toolset, AssetVfsView, AssetReadPort, Runtime, TaskScope, Selection,
  UISession and immutable SceneMeta capabilities.
- `EditorSceneHandle {slot,generation}` replaces the old generation-only identity without an alias.
- `modules/function/graph` owns stable IDs, structural topology and layout. Material/FlowForge retain their typed
  payloads and compiler IRs.
- Graph editing, read-only render protocol and Default ImGui renderer are separated. Material/Flow domain rules and
  presentation are separate L5 integration leaves. Dynamic Sequence pin actions preserve the exact PinId through
  undo/redo and use the same inverse journal as other edits.

## Verified MUST NOT items

No Host/Product architecture layer, singleton VFS/Context/Runtime, service locator, JobSystem, ProcessSender,
AssetIndex, SelectionRegistry, universal graph payload, compatibility shim, runtime-reflection Inspector fallback or
source persistence format was added. Shared graph remains ImGui/compiler/domain-semantic free. EditorApplication and
Graph domain adapters link no L4 compiler.

## Build and test gates

Every full build used `all -j 4 -k 0`.

```text
Default Developer: 160/160 CTest passed
PLAYER:            160/160 CTest passed
EDITOR:            168/168 CTest passed
TOOLCHAIN:         153/153 CTest passed
Full Render:       176/176 CTest passed
```

Full Render includes Vulkan texture/model qualification, L1-to-L3 render sync and large-scene performance. The six
race-sensitive VFS/Execution/TaskScope/AssetRead/Graph tests each passed 50 consecutive repeat-until-fail runs.

After the final CMake change, Developer, PLAYER, EDITOR, TOOLCHAIN and Full Render each received a second full build
that reported `ninja: no work to do`.

## Artifact equivalence

The canonical TOOLCHAIN configuration reproduces the saved Pre-L5 bytes exactly:

```text
605ebb80-8566-4f2a-80b6-a153fead1eec.luxasset
  DAA45C90CBF984ACCF0E6A7DE2A1D8E9060BD821CAEC8923E6423004B373037A
ccf44e8f-2220-459f-81a8-566d5c6d4b26.luxasset
  D49B59DE705992068D5CDE6A26293744098082FFDF5E6174B021A790E19B2231
ce5f070e-f086-49da-b516-bf6febd75a0f.luxasset
  DD12ACB0AE8DF4DD309F1E9037AE7808A8A3F36A80A0EF2D58A195E194F52197
static_model.luxpak
  7728D9C0E6A5BBD6E7FCC962E5B7F628CF8F40DA84A1580AF766E4F0DD08703B
```

## Installed/public closure

The canonical Editor and Toolchain install prefixes were rebuilt. Fresh relocated consumers configured, built and
ran for AssetVfsView, Process Execution, Process Asset Loading, EditorContext, shared Graph, Material Graph, Node Graph
Editor, Material/Flow Graph adapters, FlowForge model/compiler and Material compiler/cooker.

Debug, RelWithDebInfo and Android install include prefixes were synchronized for all changed `modules/*` public
headers. Android itself was not configured or built.

Closure checks show:

```text
PLAYER: no Editor or Toolchain compiler target
EDITOR: EditorApplication/Context/Graph adapters; lux_editor imports no Material/FlowForge compiler DLL
TOOLCHAIN: Material/FlowForge compilers and cookers; no Editor target
```

## Remaining STOP conditions

- C/D/E and S are intentionally outside this completed tranche.
- Material durable open/save remains blocked on source codec/document identity.
- FlowForge durable open/save/packaging remains blocked on FlowGraph codec and stable ScriptSymbol source identity.
- Project-specific target generation remains blocked on an approved project manifest/target-generation spec.
- Plugin hot unload remains held.
