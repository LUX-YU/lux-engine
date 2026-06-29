# LUX-ENGINE

Layered experimental / learning-oriented game engine (Rendering & Architecture playground).

> [中文文档 / Chinese Documentation](./doc/readme.zh-CN.md)

**Early stage project** (basic_version_dev branch). Focus is to explore modern engine architecture & real-time rendering while keeping modules cleanly layered: platform abstraction, event + reflection, resource & serialization, ECS + scripting, render pipeline (forward / deferred), visual flow (FlowForge), with future physics, animation, AI, tooling.

## 1. Architecture Overview

Top level is divided into Layers. Each layer exposes Components (CMake components) discoverable via find_package so higher layers depend declaratively.

**Runtime layers (`modules/`):**
1. **platform**: low-level platform & graphics API (common, gapi, window, dynamic_library)
2. **core**: foundational utilities (math, meta/reflection, script)
3. **resource**: asset system (description, asset: model / texture / material / shader / script asset & (de)serialization)
4. **function**: higher-level engine features (render, flowforge, gameplay, ui)

`function::ui` is a reusable ImGui UI framework — it depends only on imgui + render +
window + meta, so it can be linked on its own to build standalone UI applications,
without dragging in flowforge / script / asset / MLIR.

**Editor tier (`engine/`):** the lux-engine editor, built on top of the runtime layers.
It is not a `modules/` layer — `engine/` depends on `modules/*`, never the reverse.
* **`engine::editor`** — engine-coupled editor panels (node graph editor, lua console, asset browser) + shell
* **`engine::flowforge_compiler`** — FlowForge → MLIR → LLVM offline compiler; opt-in via `LUX_ENABLE_FLOWFORGE_MLIR`; the only target that links MLIR/LLVM
* **`asset_pipeline`** — the `lux_asset_packer` build-time CLI tool (links only `resource::asset`; built unconditionally because the runtime build itself depends on it)

Set `-DLUX_BUILD_EDITOR=OFF` for a runtime-only build (excludes `engine::editor` and
`engine::flowforge_compiler`).

**Rendering (function/render)**
* Vulkan backend (through gapi + window)
* GLSL to SPIR-V compilation (glslc) integrated in build
* Forward & deferred shader sets (skeleton)
* Asset conversion hookups (AssetConverter)

**Scripting (core/script)**
* LuaJIT runtime integration
* Linked MLIR libs (IR, Dialect, Parser, Pass) for future DSL / IR experiments

**Reflection & Serialization**
* Powered by the [lux-cxx](../lux-cxx) toolchain. `reflection` (libclang-based codegen → compile-time `meta_info` + runtime `MetaUnit`) drives type metadata.
* The [**serialization**](../lux-cxx/serialization/README.md) module turns any annotated type into **JSON / XML / command-line** with a single `LUX_META(serializable)` annotation — no per-type code, no third-party headers leaking into the build. Used for assets, config, and editor inspectors.

**FlowForge (function/flowforge)**
* Early IR node declarations hinting at a node/graph driven logic editor (not yet functional)

---

---

## 2. External Dependencies

**Toolchain:**
* CMake >= 3.22
* C++20 compiler (Clang / GCC / MSVC)
* Ninja (recommended) or Make

**Third-party libraries:**
* **Vulkan SDK** (with glslc) — rendering & shader compilation
* **GLFW3** — window & input
* **Eigen3** — math
* **Assimp** — model import
* **stb** (header-only image loading) — must be available in include path or vendored
* **stduuid** — UUID generation
* **LuaJIT** — scripting runtime
* **MLIR** (part of LLVM) — planned IR / compilation pipeline experiments
* **fmt** (optional; probed quietly)
* **PkgConfig** — assists library discovery

**Project-internal (installed separately):**
* **lux-cmake-toolset** — macro helpers (generate_visibility_header, add_component, etc.)
* **lux-cxx** — custom metaprogramming, reflection & serialization (compile_time, reflection, serialization). Reflection-driven JSON/XML/CLI serialization: see [serialization docs](../lux-cxx/serialization/README.md)

These must be installed so `find_package(... CONFIG)` succeeds, or adapt CMake to `add_subdirectory` them locally.

---

## 3. Directory Structure

```
modules/       (runtime layers — what a shipped product may link)
  platform/    (common, gapi, window, dynamic_library)
  core/        (math, meta, script)
  resource/    (description, asset)
  function/    (render, flowforge, gameplay, ui)
engine/        (editor tier — depends on modules/*, never the reverse)
  editor/             (engine-coupled editor panels + shell)
  flowforge_compiler/ (FlowForge MLIR/LLVM compiler; opt-in)
  asset_pipeline/     (lux_asset_packer build-time tool)
spir_v/        (SPIR-V shader outputs)
cmake/         (Find*.cmake & helper scripts)
```

---

## 4. Build (Linux Example)

Install: Vulkan SDK, glfw3, Eigen3, Assimp, LuaJIT, MLIR, stduuid, pkg-config, (fmt optional). MLIR often requires building LLVM with MLIR enabled.

**Illustrative (adjust per distro):**
```bash
# Install dependencies (example Ubuntu - actual packages may vary)
sudo apt install build-essential ninja-build cmake pkg-config \
  libvulkan-dev vulkan-tools glslang-tools \
  libglfw3-dev libeigen3-dev libassimp-dev \
  luajit libluajit-5.1-dev

# stduuid (via package manager, vcpkg, or manual installation)
# fmt (optional)

# MLIR: usually requires building from LLVM source (provide MLIRConfig.cmake)

# Build & install lux-cxx + lux-cmake-toolset beforehand if not present
# git clone ... && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build --target install

# Build this project
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Optional: install
cmake --install build --prefix /your/install/prefix
```

**Windows / macOS:** use corresponding Vulkan SDK; if MLIR is heavy and scripting is not needed initially, temporarily disable the script component in CMake.

---

## 5. Run / Quick Check

No consolidated demo executable yet. Optional test targets behind CMake options (e.g. `ENABLE_RENDER_TEST`, `ENABLE_SCRIPT_TEST`). Enable then build:

```bash
cmake -B build -G Ninja -DENABLE_RENDER_TEST=ON
cmake --build build
```

Shaders auto-compile (glslc) to SPIR-V into `spir_v/` or the configured output directory.

---

## 6. Current Completion Snapshot

| Module | State | Notes |
|--------|-------|-------|
| platform::window | Basic window + Vulkan flags | Input & multi-platform incomplete |
| platform::gapi | Vulkan macro layer | Missing abstraction & backend switching |
| platform::event | Interface only | Lacks dispatcher implementation |
| core::math | Interface + Eigen | Needs transforms, SIMD optimizations |
| core::meta | (not shown here) | Must integrate with assets, scripting, editor |
| core::script | LuaJIT + MLIR linked | Missing VM mgmt, binding, hot reload, MLIR pipeline |
| core::ecs | Referenced only | Not implemented (storage & scheduling) |
| resource::asset | Asset types + (de)serialization skeleton | Missing ref counting, async loading, cache, dependency graph |
| function::render | Pipeline skeleton + shader build | Missing frame/render graph, queues, material system, deferred lighting impl |
| function::flowforge | Early IR headers | Needs node system UI, execution, save/load |
| function::ui | ImGui UI framework (panels, docking, widgets, ImGui<->render bridge) | Reusable runtime-layer component; standalone-linkable |
| engine::editor | Engine-coupled editor panels (node editor, lua console, asset browser) | ImGui-based; gated by LUX_BUILD_EDITOR |

**Overall:** Core runtime pillars (ECS, frame graph, resource streaming, reflection-editor loop) still WIP.

---

## 7. Roadmap / TODO

### Short Term
1. Implement **core::ecs** (entity/comp storage, system scheduling, event bridge)
2. Expand **event** into unified bus + input subsystem, ECS integration
3. **Rendering**: frame graph / render graph abstraction; forward + deferred fleshed out (GBuffer, lighting); descriptor & material parameter system
4. **Resource pipeline**: async loading (job system), ref counting, dependency tracking, caching (LRU)
5. **Reflection integration**: unified type registry powering serialization + inspector
6. **Scripting**: LuaJIT VM mgmt, ECS bindings, define MLIR direction (JIT optimizations vs DSL)

### Mid Term
7. **Editor (`engine::editor` + `function::ui`)**: ImGui docking workspace & panels (scene, assets, inspector, logs, render debug, FlowForge)
8. **FlowForge**: node definitions (reflection-driven) → visual editing → serialization → runtime execution
9. **Rendering features**: PBR, IBL, shadows, post-processing (Bloom, TAA, ToneMap), GPU profiler
10. **Multi-platform**: Win/Linux parity; consider macOS via MoltenVK
11. **Offline asset pipeline**: preprocessing, compression, shader reflection artifacts

### Long Term
12. **Physics** (Bullet/PhysX or custom), **animation** (skeletal, blend trees), **audio**, **AI** behavior trees
13. **Hot reload** (assets, scripts, shaders), incremental asset builds
14. **Multithread job system** orchestrating render & streaming
15. **Unified scheduler**: ECS + frame graph + resource streaming
16. **Plugin system** / modular runtime loading
17. **Networking** experiments (optional)

### Engineering / Quality
18. **Unit & integration tests** (enable tests more directly)
19. **Static analysis** (clang-tidy), coverage, sanitizers, CI
20. **Documentation & samples** (minimal demo, scripting sample, FlowForge example)

---

## 8. Contribution Suggestions

* Focus on one layer at a time (e.g., ECS or frame graph) to avoid diffusion
* Maintain dependency direction (platform → core → resource → function; the `engine/` editor tier sits above and depends on `modules/*`, never the reverse)
* Each component should gain: rationale doc, minimal sample, public header inventory
* Make shader / script generation reproducible (document paths, add helper scripts)

---

## 9. License

Not specified yet (recommend choosing MIT / Apache-2.0 etc. early for clarity).

---

## 10. Status

Early exploratory project; APIs / layout subject to change. Not production ready.

---

Contributions / issues / discussions welcome. If you're building a learning engine too, sharing evolution notes & benchmarks is valuable.

*(README auto-generated & translated from current CMake/layout snapshot; please update as structure evolves.)*

