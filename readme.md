# lux-engine

`lux-engine` is a modern C++20 game-engine project focused on explicit ownership,
deterministic ECS scheduling, asynchronous asset streaming, and a Vulkan renderer.
The repository is intentionally split by **responsibility** and **shipped product**:
runtime code cannot depend on authoring, toolchain, or editor code.

> [中文说明](./doc/readme.zh-CN.md) · [active architecture work](./.internal/UNFINISHED-WORK.md)

## Architecture

The enforced source/target dependency DAG is:

```text
Platform -> Core -> Resource -> Function -> ECS -> Runtime -> Host
                         \-> Authoring -> Toolchain -> Editor
                                      Runtime ----^       ^
```

More precisely:

- `modules/platform` wraps OS, window, dynamic-library, file-watch and graphics-API entry points.
- `modules/core` contains domain-neutral values and infrastructure: bytes,
  serialization, events, logging, math, meta and the extension ABI.
- `modules/resource` contains cooked runtime resource descriptions, asset identity,
  the asset ledger, runtime codecs and pak reading.
- `modules/function` contains reusable non-ECS domains such as rendering, scripting,
  input, UI and FlowForge runtime vocabulary.
- `ecs` contains components, `ISystem`, `IRenderSubsystem` and ECS-domain adapters.
- `engine/runtime` composes asynchronous execution, assets, scenes, rendering,
  extensions, frame coordination and runtime packs.
- `engine/authoring` contains editable source documents and project data.
- `engine/toolchain` converts authoring data into cooked runtime data. Assimp,
  shaderc, SPIR-V reflection and MLIR/LLVM belong here and never in Player.
- `engine/editor` contains editor-only controllers, panels and frameworks.
- `engine/hosts` contains composition roots and main loops only.
- `extensions` contains deployable `MODULE` libraries. Engine targets never link a
  concrete extension implementation.

CMake targets declare `LAYER`, `PRODUCT`, and `ROLE`. Configuration fails on an
illegal classified dependency, an unclassified production target, or a concrete
extension linked back into the engine.

## Runtime model

- A scene owns a `World`, a topologically compiled `Schedule`, and at most one
  top-level `RenderSystem`. Omitting the render pack produces a headless scene.
- Frame, control, and persistent GPU upload use separate channels. A single transfer
  pipeline owns transfer recording/submission without a queue-submit mutex.
- `AsyncRuntime` uses registration-owned typed bounded queues, one standalone-Asio
  coordinator, a small blocking-I/O compatibility executor, and oneTBB for CPU work.
- `MainThreadMailbox` is the only cross-thread completion path into ECS,
  `AssetManager`, UI state, and `DomainEvents`.
- `DomainEvents` broadcasts already-committed facts; it is not a command bus or a
  request/reply mechanism.
- Runtime extension is split into module loading, contribution registration, and
  per-world/render-scene/editor activation. Type identity does not use RTTI.

The codebase forbids RTTI and exception-driven control flow in engine code. Fallible
APIs use `expected`, ownership is expressed with RAII/owning packets, and hot paths
avoid locks and unbounded queues.

## Render boundaries

Rendering has four CMake components:

- `lux::engine::function::render_client`: backend-neutral handles, protocols,
  channels and Frame/Control/Upload sessions; public headers contain no Vulkan types.
- `lux::engine::function::render_graph`: logical resources, passes, dependency
  analysis and compiled logical plans; it can be tested without a Vulkan device.
- `lux::engine::function::render_vulkan`: Vulkan server, resource managers, graph
  lowering, queues and `GpuTransferPipeline`.
- `lux::engine::function::render_features`: built-in Vulkan feature implementations
  and their generated assets.

ECS extraction and `FrameCoordinator` depend only on `render_client`. A headless
`SceneRuntime` does not link the Vulkan backend.

## Build profiles

Configure with `LUX_BUILD_PROFILE`; the removed `LUX_BUILD_EDITOR` switch is not
supported.

| Profile | Contents |
|---|---|
| `DEVELOPER` | Runtime, Player, Editor and Toolchain |
| `PLAYER` | Runtime and the reference Player; native or cross-compiled |
| `EDITOR` | Runtime, Editor and Toolchain; no reference Player executable |
| `TOOLCHAIN` | Offline tools and their minimum dependencies |

The target platform comes from the CMake toolchain. For example, Android uses
`LUX_BUILD_PROFILE=PLAYER` plus the Android triplet and an explicit
`LUX_HOST_TOOLS_PREFIX`; platform names are not product profiles.

Example with Ninja and vcpkg:

```powershell
cmake -S . -B ../build/RelWithDebInfo/lux-engine -G Ninja `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DLUX_BUILD_PROFILE=DEVELOPER `
  -DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build ../build/RelWithDebInfo/lux-engine --target all -j 4 -k 0
ctest --test-dir ../build/RelWithDebInfo/lux-engine --output-on-failure
```

The sibling projects `lux-cmake-toolset`, `lux-cxx`, `imgui`, and
`imgui-node-editor` must be discoverable through `CMAKE_PREFIX_PATH`. The bootstrap
graph in `bootstrap/` can build that chain on a fresh machine.

Do not run a build concurrently with device/attended validation. After a CMake
change, build twice; the second pass must report no work. Changes to public
`modules/*` headers must be synchronized to the Debug, RelWithDebInfo, and Android
install prefixes because meta generation reads installed headers.

## Products and export

The primary executables are:

```text
lux_player
lux_editor
lux_launcher
lux_asset_packer
lux_shader_emitter
lux_game_exporter
```

`lux_game_exporter` emits a cooked runtime directory containing the Player,
runtime manifest, pak files, runtime libraries, and runtime extensions. Player does
not support loose authoring/project content. Export rejects authoring-only payloads
or Editor/Toolchain binaries in the runtime closure.

Install/CPack components distinguish reusable layers from products, including
`lux_runtime`, `lux_player`, `lux_editor`, `lux_toolchain`, and `lux_sdk`.

## Current status

The large directory/target migration is active. The product profiles, dependency
classification, runtime/editor host split, Render four-way target split, asset
identity/core/codecs/pak split, authoring/toolchain trees, extension module, and
runtime inventory/export checks are implemented. Some physical source splits and
full platform validation remain; the authoritative list is
[`.internal/UNFINISHED-WORK.md`](./.internal/UNFINISHED-WORK.md).

## License

See the repository license files.
