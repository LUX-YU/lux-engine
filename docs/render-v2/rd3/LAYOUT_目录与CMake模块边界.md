# RD3 目录与 CMake 依赖蓝图（完整目标，非立即迁移清单）

> 与 02、09、14 互为交叉索引。本文件用**真实职责分层**而非为每一个名词先建一个 CMake target。所有示例文件名/namespace 是目标建议，不代表 Git HEAD 已存在。新建每个类需要真实消费者，不允许一次性生成空壳目录。

```text
modules/function/render/
├── core/
│   └── include/lux/engine/render/core/        # RenderResult / typed IDs / portable descriptors
├── transport/
│   ├── include/lux/engine/render/transport/   # 现有 PROGRAM CONTROL UPLOAD / Reply / routes
│   └── src/
├── client/
│   ├── include/lux/engine/render/client/      # RenderRuntime.hpp, typed client requests
│   └── src/                                  # protocol packing, no Vulkan headers
├── data/
│   ├── include/lux/engine/render/data/        # DataSchema<T>, optional GPU/Wire representation
│   └── src/                                  # ABI/schema validation, existing generator consumers
├── scene/
│   ├── include/lux/engine/render/scene/       # Scene.hpp View.hpp (neutral)
│   └── src/                                  # ownership + simple semantic state
├── target/
│   ├── include/lux/engine/render/target/      # Target.hpp TargetDesc.hpp TargetLayer.hpp
│   └── src/                                  # neutral output/layer contracts only
├── resource/
│   ├── include/lux/engine/render/resource/    # ResourceDomain.hpp ResourceRegistry.hpp
│   └── src/                                  # generational registry/parent lookup, no VkMemory
├── effect/
│   ├── include/lux/engine/render/effect/      # RenderEffect.hpp Feature.hpp FeatureOps.hpp
│   └── src/                                  # revision/configuration, cold build/ABI
├── graph/
│   ├── include/lux/engine/render/graph/       # RenderGraphBuilder.hpp Definition.hpp IR.hpp
│   └── src/                                  # C0-C8, hazard/version/liveness/topo, cold only
├── backend/
│   ├── include/lux/engine/render/backend/     # RenderBackend.hpp (neutral server authority)
│   ├── src/                                  # one frame cadence, scene/effect/graph/domains/targets
│   └── vulkan/
│       ├── include/lux/engine/render/backend/vulkan/
│       │   ├── Backend.hpp                   # owns actual VkDevice/Allocator/Queues/Retirement
│       │   ├── Graph.hpp                     # immutable native compiled program
│       │   ├── Compiler.hpp                  # IR → native program
│       │   ├── Executor.hpp                  # execute graph, view(s), target
│       │   └── GraphExecutionContext.hpp     # Context with per-job/FIF native slots
│       ├── src/
│       │   ├── device/                       # VkInstance/device enabled caps
│       │   ├── resource/                     # Buffer Image ImageView VMA + Native Stores
│       │   ├── shader/                       # LayoutPlan, SPIR-V, PSO owner cache
│       │   ├── graph/
│       │   │   ├── Compiler.cpp              # native fixed pipeline orchestrator
│       │   │   ├── Resources.cpp             # C10 physical planning / alias
│       │   │   ├── Recipes.cpp               # C11 binding/pipeline/cmd recipes
│       │   │   ├── Synchronization.cpp       # C12 Sync2 + queue evidence
│       │   │   ├── Executor.cpp              # native record/submit work
│       │   │   └── Context.cpp               # slots, imports/ready, actual barriers, tickets
│       │   ├── target/
│       │   │   ├── Offscreen.cpp             # image/readback/backing retention
│       │   │   └── Swapchain.cpp             # Surface acquire/present/resize fencing
│       │   └── transfer/                     # staging, Queue owner, submission/retire
│       └── test/
├── features/                                  # only real business feature build algorithms
│   ├── mesh/
│   ├── material/
│   ├── lighting/
│   ├── shadow/
│   ├── postprocess/
│   ├── canvas/
│   ├── point_cloud/
│   ├── robotics/
│   └── ...
└── sdk/                                       # thin installed public/Plugin C ABI exports; may be
                                               # folded into effect/data public headers

engine/scene/builtin_systems/render/
    RenderSystem, ECS deltas, Client scene/view/asset handles, no backend private include
engine/toolchain/shader/
    existing GLSL/SPIR-V and meta single source (no second parser)
```

## CMake 推荐边界

独立 target 只在头文件 public install/链接生命周期明确需要时创建：`render_core`, `render_transport`, `render_client`, `render_graph`, `render_data`, `render_backend`（中立 owner）与 `render_backend_vulkan`（Native）；`scene`, `effect`, `target`, `resource` 的中立实现可按真实依赖合并到相邻目标，**不必每个目录建立静态库**。后端未来 `render_backend_d3d12`、`render_backend_gl` 按真正实施时增加；旧 `render_vulkan` 功能迁移按组件测试/资格，不并行装一个“Vulkan 旧 Backend + Vulkan 新 Backend”长期模式。

依赖边：`client -> transport/core`，`graph -> core/data/description`，`backend(neutral) -> scene/effect/target/resource/graph/transport`，`backend_vulkan -> backend(neutral) + graph + render native foundation + shader toolchain`。公开安装 SDK 不含私有 `pinclude/sinclude` 和 `vulkan/vulkan.h`；Native 特殊插件走 capability-based 单独授权合同。

## 文件迁移与删除的判断标准

`RuntimePathState/RuntimeNativeVariant/NativeGraphInvocations/OwnerShape` 等旧名称允许在施工过渡期间短期保留，以确保可编译、保持原测试；**最终权威类型必须明确移到新 Owner，并删除长期旧平行模型**。实施方若留旧 struct 作永远转发容器，必须在 gate 判 FAIL。每次文件移动都提供 include/link DAG、source SHA、原 GPU tests/bench affected scope，并禁止把文件行数降低当成代码质量证据。
