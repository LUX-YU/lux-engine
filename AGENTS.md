# lux-engine 工程约定

本文件的主体记录**可机检、会反复被违反**的结构规矩。每一条都来自一次真实的返工，
后面附着它的成因——不写成因的规矩会在下一次"就这一次"里被绕过。
前两节（项目簇背景、代码风格）是用户定的基线约定，随仓走。

（设计决策与未完成工作不在这里：前者在 `.internal/*.md` 的各份 ADR，
后者在 `.internal/UNFINISHED-WORK.md`。）

---

## 项目簇背景（lux-\*，同级文件夹）

lux 是一个项目簇；lux-engine 是其中的游戏引擎——`modules/` 提供可被外部项目
复用的基础应用功能，`engine/` 提供引擎专属功能。编译与执行入口见
`.vscode/launch.json` 与 `.vscode/settings.json`。

- **lux-cmake-toolset**：cmake 基础工具（构建组件/安装），基础组件。
- **lux-cxx**：C++ 基础组件（编译期工具、反射、异步、容器、序列化等），
  基础组件，功能清单见其 README.md。
- **imgui**（修改版）：兼容 lux-engine 的多线程 UI 渲染 + cmake 构建。
- **imgui-node-editor**：节点可视化（材质图、流程图、图形化编程）。
- **lux-communication**：模仿 ROS2 的通信库。
- **lux-dataset**：主要用于支持机器测试。
- **lux-robotics**：机器人库（正在构建 SLAM 部分），依赖 lux-engine 做图形化。

## 代码风格

### 基本排版
- 单条语句在不超过 120 列时保持一行；只有超过 120 列或层次明显影响可读性时才换行。
- 换行应表达语义层次，不要为了凑列宽把一个完整表达式拆成难以阅读的碎片。
- 多行列表、初始化和调用的结束括号/花括号独占下一行，并与对应的开始层级对齐。

### 命名
- **类名 / struct（含纯数据类型）**：首字母大写驼峰 `XxxYyyy`。
- **成员变量**：小写下划线 `xxx_yyy`；private 成员加末尾下划线 `xxx_yyy_`。
  成员类型太长时用 `using` 起别名，保持声明在列上对齐。
- **成员函数**：小写驼峰 `aaaBbbb()`。
- **枚举**：一律 `enum class`，类名 E 前缀驼峰 `EXxxYxx`；
  成员全大写下划线 `XXX_YYY_ZZZ`（存量有很多不遵守的，改到就顺手正名）。

### 长语句换行
函数调用超过 120 列时逐参数换行、**闭括号放到下一行并保持层级**；参数尽量同一行，
单个参数过长再继续拆分；过长先考虑是不是设计问题：

```cpp
xxx::object.invoke(
    aaaa,
    bbbb.make_object(
        dddd
    ),
    cccc
)
```

函数声明过长时，优先把限定符、返回类型和可见性放在一行，函数名另起一行；
如果返回类型本身过长，先用 `using` 起一个有意义的结果类型别名：

```cpp
using LuaScriptResult = lux::cxx::expected<LuaScriptBindingBackend, ELuaScriptBindingBackendError>;

[[nodiscard]] static LuaScriptResult
create(std::size_t instance_capacity, std::span<const LuaComponentBinding> components = {}) noexcept;
```

如果参数列表仍超过 120 列，再按参数逐行换行：

```cpp
create(
    std::size_t instance_capacity,
    std::span<const LuaComponentBinding> components = {}
) noexcept;
```

多行聚合初始化的最后一个元素与结束花括号分行：

```cpp
result = Type{
    first,
    second,
    {}
};
```

### 复杂判断
- `if`、`else if` 或循环条件包含多个相互独立的校验时，先在判断前用具名 `const bool`
  表达各个语义分组，再组合成最终结果；变量名应说明是 `is_invalid_*`、`has_*`、
  `is_*_mismatch` 等什么条件。
- 最终判断只保留聚合结果，例如 `if (is_invalid_descriptor)`；不要把一长串字段比较直接
  堆在条件中。
- 外提判断不得破坏原有短路安全性。涉及指针、迭代器、范围边界或可能溢出的算术时，
  先建立前置有效性布尔量，再在有效时计算后续条件；需要时保留分阶段的 `&&` 短路。

```cpp
const bool is_invalid_type = !descriptor.type ||
    descriptor.type != AssetTypeId::fromName(descriptor.canonical_name);
const bool is_invalid_magic = descriptor.primary_magic == 0u;
const bool is_invalid_cpp_type = descriptor.cpp_payload_type.hash() == 0u ||
    descriptor.cpp_payload_type.name().empty();
const bool is_invalid_decode = descriptor.decode == nullptr;
const bool is_invalid_encode = descriptor.encode == nullptr;
const bool is_invalid_descriptor = is_invalid_type ||
    is_invalid_magic ||
    is_invalid_cpp_type ||
    is_invalid_decode ||
    is_invalid_encode;

if (is_invalid_descriptor)
{
    return lux::cxx::unexpected(EAssetCodecError::INVALID_DESCRIPTOR);
}
```

### 模块结构（lux-cmake-toolset 构建的模块）
- `include/`——安装的头文件，全局公开;
- `sinclude/`——项目级可见，不安装;
- `pinclude/`——仅模块内可见，不安装;
- `src/`——源文件，内部结构一般与头文件一致。

---

## 目录、Target 与产品边界

SSOT 见 `.internal/directory-target-product-architecture.md`。目录、CMake target 和安装产品
必须同时表达同一职责，禁止只搬文件而保留反向链接，或只改 target 名而让源码继续跨层 include。

- Canonical ontology 固定为 `L0 Modules -> L1 Domain foundation + World + Simulation -> L2 Process ->
  L3 Scene -> L4 Toolchain -> L5 Editor`。Product/Application composition 是 build closure 维度，不是架构层；
  L5 `EditorApplication` 可直接产出 Editor executable，最终游戏由另行批准的 project-specific target generation
  组合。World 描述事实；Simulation
  同步解释和运行事实；ECS 只是 Simulation 内部机制；Process 负责异步、IO 与 residency；
  Scene 只组合 World 与 Simulation。不得恢复顶层 `engine/ecs` 或 `ECS` classifier。
- `World = durable/cooked facts + whole-world storage metadata`；`Simulation = concrete Systems +
  synchronous rules + compiled schedule`；`Scene = one World + one authoritative Registry + one Simulation`。
  SceneSystem 只承担跨 Simulation/Process/application capability 的 L3 composition；由 owning `SceneDescription`、
  immutable `SceneMetaManager` 与串行 stable/presentation hooks 安装，不得创建第二个 Scene TaskGraph。
  Streaming policy 只属于 concrete developer System；Presentation 是可独立采样的 runtime concern，
  不是 architecture layer。Canonical Transform2D/3D 与 WorldTransform2D/3D 一律使用 double。
- source topology 固定用 `engine/domain/{partition,spatial,system,world,simulation}` 表达 L1 ownership：
  `partition/identity` 与 `world/identity` 是 DOMAIN-classified 的窄义 identity leaves，World 的
  `partition/description/storage/asset` 与 Simulation 的 `description/asset/composition/ecs/system/builtin/scripting`
  按语义职责拆包；
  用 `engine/editor` 与 `engine/toolchain` 区分交互工具和离线工具；不得恢复同义叠加的
  `engine/tools`。public include 与 namespace 仍使用概念名，不得把 `domain` 或层级编号写进用户 API。
- `DOMAIN` 只分类引擎专属、被 World/Simulation 共同依赖的窄义 L1 foundation；不得建立
  `domain/common`、`domain/utils` 或 `domain/services`。`WORLD` 与 `SIMULATION` 是 L1 sibling roots。
  `engine/domain/system` 只拥有跨 SimulationSystem/SceneSystem 共用的 stable type/instance identity、
  type description 与 deterministic dependency-order helper；不得拥有 runtime object、scheduler、registry或codec。
  `world/partition` 不依赖 WorldDescription；`world/description` 只组合 World identity、partition identity 与
  partition contracts；`world/asset` 才能增加 L0 Asset/Serialization closure。
  Simulation 可依赖 DOMAIN 与 L0，但不得依赖 World，
  不得依赖 Process、Scene、Toolchain 或 Editor。旧 `RUNTIME` classifier
  只对白名单存量 extension 暂留；新 L2/L3 target 必须分别使用 `PROCESS/SCENE`。
- 所有 production target 必须调用 `lux_classify_target(TARGET ... LAYER ... PRODUCT ... ROLE ...)`。
  不得靠未分类 target、`INTERFACE_LINK_LIBRARIES` 或 generator expression 绕过 DAG 门禁。
- `LUX_BUILD_PROFILE` 只有 `DEVELOPER/PLAYER/EDITOR/TOOLCHAIN`；Android 使用 `PLAYER`
  配合 Android toolchain/triplet；不得恢复 OS-shaped profile，也不得恢复
  `LUX_BUILD_EDITOR`。`PLAYER` 仅是 runtime-clean qualification profile，不是最终交付产品身份；
  project manifest/target-generation contract 未批准前不得发明 generic Player/Host framework。
- BUILD_TOOL 关系只用 `lux_add_build_tool_dependency()`、custom command 与 generated file；
  shader compiler、asset packer、meta generator 不得作为 Runtime link dependency。
- `engine/process/execution` 是领域盲基础设施；`engine/process/world_loading` 与 `engine/process/asset_loading` 可拥有明确的
  time-spanning workflow。领域workflow不得反向进入 execution，也不得依赖 Scene、Render 或 gameplay policy。
  `TaskScope` 只接管 lifetime：交入 `start()` 前必须已消费业务 value/error，使 sender 只剩无载荷
  `set_value()` 与可选 `set_stopped`。start/stop/close admission 必须线性化，但不得持 TaskScope 自身锁调用
  eager sender、`async_scope::spawn/request_stop`、stop callback、receiver 或用户代码。
- `engine/scene/composition` 组合 Scene ownership，`engine/scene/presentation` 承载 independently sampled state，
  `engine/scene/integration/{world_materialization,render}` 只拥有对应的 L3 integration；不得恢复历史
  `scene/core` 或 `scene/runtime` 聚合目录。
- Render 固定为 `render_client/render_graph/render_vulkan/render_features` 四 target。
  `RenderSystem` 是可选的 concrete SceneSystem，要求 application composition 提供 `RenderRuntime` capability；
  它不创建 thread/device，
  只冷装 RenderScene/Feature/feature-owned stages，并通过 `RenderSyncPipeline` 在 stable point发布 StateUpdate、
  在 Presentation 转发。无 RenderSystem 的 Scene 不创建 RenderScene；无 render integration pack 的 Scene 必须 headless。
- `Authoring` 不再是 architecture layer，`engine/authoring` 不得恢复。可编辑 source model 位于其可复用的
  semantic package（例如 `modules/function/material` 与 `modules/function/flowforge`）；L4 Toolchain执行
  source/import→compiled/cooked/package，L5 Editor只负责交互式编辑并调用L4 public facade。Player只读取
  RuntimeLaunchManifest与cooked pak；MaterialGraph、compiler/cooker、Assimp、shaderc、spirv-cross、MLIR/LLVM、
  GraphKit和Editor UI不得进入Player闭包。
- Material 固定为 `MaterialGraph -> compileMaterial() -> MaterialDescription -> MaterialAsset`；Graph source位于
  `modules/function/material`，compiler/cooker位于`engine/toolchain/material`，private MaterialIR/ShaderIR不得安装。
  Compiler只使用build-time embedded的canonical Shader include map，installed compiler不得读取原source/build tree；
  shared SPIR-V reflection与LGLSL emitter属于`engine/toolchain/shader`，package不得跨 sibling `pinclude`。
  `engine/editor/node_graph`只提供domain-independent编辑机制，不得成为source model或compiler owner。
  GraphKit compound edit/undo/redo使用原子inverse journal：失败必须恢复document/history/revision，且
  `node_graph_editor`固定分类为EDITOR layer。
- Extension 是叶节点。引擎 target 不链接 `extensions/*`；manifest 决定部署和运行期加载。
- 旧 include、target 或类型搬迁不得留 shim/alias。历史扁平目录名不得重新创建。

---

## 头文件

### 搬迁必须一次做完，不留兼容别名

把一个类型从 A 命名空间搬到 B 之后，**不要**在 A 留一个
`namespace A { using B::T; }` 的转发头。

**成因**：`engine/editor/.../project/Project.hpp` 曾经就是这么一个 shim（现已删除），
而它的注释亲口写着——"因为存在 using 别名，不能写前置声明，只能把 shim 拉进来"。
也就是说 **shim 自己造出了必须 include 它的理由**：下游本来只需要一个
`class Project;` 前置声明，却被迫拉进整个头。删掉别名之后前置声明立刻可用。

若确实需要过渡期，在 shim 里加 `[[deprecated]]`——**让编译器代替人类计时**。

⚠️ **不要误伤**给匿名类型起名的正当命名头（`ecs/core/.../AssetLoadFn.hpp`、
`render/.../gpu/utils/Slot.hpp`）——它们不是搬家残留。

### `using` 只用于把**别的**命名空间的名字引进来

`namespace lux::ecs { using lux::ecs::World; }` 是个 no-op。全仓曾有 30+ 处这样的
自指 `using`，它们看起来像"这个头提供 World"，实际什么都没做——真正提供它的是
上面那行 `#include`，而那行 include 往往也是死的（组件头里一次都没用到 `World`）。

判据：`using` 的目标命名空间 ≠ 当前命名空间才写。
`engine/editor/.../controllers/EditorCamera3DController.hpp` 里的
`using lux::input::ActionMapper;` 是合法的。

### 组件头只许 include 字段类型与反射标注所需的头

**不许** include 任何 bridge / system / feature 头。

**成因**：组件是**数据**，桥与特性是**行为**，方向必须是"行为看见数据"。
`SkyboxComponent.hpp`（一个单字段 POD）曾把 28-include 的
`RenderableBridgeContext.hpp` 整个拉进来、头内一处未用；
`TilemapComponent.hpp` 为了一个 `std::uint16_t` 常量拉进一个 9-include 的
render feature 头。

两边都要用的**编码约定**（如瓦片的 `kEmptyTile`）放到共同的下游
`modules/resource/description`（`lux::rdesc`），两边各自 include 那一份。

### 公共头不 include 只有 .cpp 需要的重型依赖

`ShaderSerDeser.hpp` 曾在头里 include `spirv_cross`，而该头声明的**每一个签名都
不含 spirv_cross 类型**，`.cpp` 自己也已经 include 了——8 个下游 TU 白付整个
SPIR-V 反射编译器的解析成本。

---

## 诊断与错误

### Lux semantic error 不使用 C++ exception

Runtime/domain public API 默认使用 `noexcept` 与 `expected`/结构化 error。Lux-owned production
代码不得主动 `throw`，Hook/Event record、dispatch、drain 与 System/Task 执行热路径不得出现
`try/catch`。STL 分配与第三方库异常只允许在 Builder、Codec、fallible factory、Toolchain
compiler 或明确的 plugin/foreign containment boundary 捕获，并必须立即转换为 Lux error；
不得让异常跨 DLL、System、Task、Script ABI 或 plugin boundary。不得为此全局启用
`-fno-exceptions` 或 `/EHs-`。

### 库不决定文字打到哪；宿主装配一次出口

见 `modules/core/log/include/lux/engine/log/Log.hpp` 的文件头（§7.1 两条通道）。
库层只声明 level + category + 消息，落点由宿主在启动时 `addSink` 决定。
`modules/function/render` 更严格：它连 `lux::log` 都不链接，诊断只走
`RenderErrorSink` 的结构化错误 + `Expected` 返回值（`no_terminal_io` 门禁）。

**推论——出口装配要有唯一实现**：同一条诊断的出口如果由每个宿主各写一遍，
它必然会漂。渲染桥的 sink 就漂过：编辑器给了 stderr、game_host 给了 lux::log、
Android 一个都没装。现在统一在
`engine/scene_runtime/.../RenderDiagnostics.hpp`，宿主只负责调用。

### 不要用 `assert` 表达"这在实机上不该发生"

实机跑的是 **RelWithDebInfo，带 NDEBUG**——`assert` 恰好在唯一重要的配置里整条消失。
要么用 `renderFatal`（render 层，见 `core/RenderFatal.hpp`），要么用一个在所有配置里
都成立的计数 + 关闭期上报。

---

## ECS 观察者（`on_construct` / `on_update` / `on_destroy`）

这一节的每一条都来自批 3（相机域）的一次真实返工。反应式改造的风险不在机制难写，
而在于**这里的每一种错法都不报错**——构建全绿、测试全过、进程干净退出，
错误只以「某个东西没渲染」「切回前台画面不动」的形式出现。

### 观察者内不得直接改世界，一律走延迟命令

EnTT 的信号在 `emplace` / `erase` 的**过程中**派发：观察者跑的时候，发信号的那个 pool
正处在一次修改的中途。给**别的** pool 加组件通常可行，但销毁实体、或碰同一个 pool
就不安全。用 `ecs/core/.../DeferredCommands.hpp`：观察者只入队，在一个已知的安全点
（`Schedule::applyCommandBarrier()`，即 tick 末尾的唯一 apply 点）排空。

排空期间**新入队的留到下一次**。命令里发信号、信号里再入队是正常链条，就地消费会变成
自喂循环，而且是那种只在特定组件组合下才发作的。

### 连接信号时必须**折入存量组件**

信号只对连接之后发生的事说话。宿主的自然写法是「建实体 → 挂组件 → 让系统开始工作」，
而连接往往发生在最后一步里——**组件早就 emplace 了，`on_construct` 永远不会为它触发。**

批 3 撞过：编辑器正常启动、干净退出、stderr 零报错，**主场景不渲染**（一个 view 都没建）。
`target all` 全绿、`ctest` 23/23 全过都发现不了；是把 stderr 与改动前那份**逐行对拍**、
发现少了 8 行诊断才暴露的。

处方见 `ecs/core/.../HierarchyView.hpp` 的 `ensureHierarchyIndex`：连信号 + 遍历已存在的
组件补一遍，两件一起做。这样「谁先谁后」不再是调用方要操心的事。

### `on_update` 只认 `patch<T>()` / `replace<T>`

直接 `registry.get<T>(e).field = x` **不发信号**。所有需要被观察到的写入必须走 patch。

真实后果：`SceneRuntime::reattachTarget` 改 `ViewPresentComponent::target` 若不走 patch，
layer 就不会重设——Android 切回前台后画面永远停在旧 surface 上，不报任何错。

### EnTT **没有** Unity 那种「cleanup 组件在实体销毁后留存」的机制

`registry.destroy(e)` 会移除**所有**组件，不存在「系统看见残留的 cleanup 组件再释放」
这一步。别照着 Unity `ICleanupComponentData` 的说法去找它。

等价做法更简单：**在 `on_destroy` 里读句柄**——信号触发时组件仍然可读，把资源句柄读出来
入队即可。这一条同时覆盖「组件被摘掉」与「实体被销毁」（EnTT 销毁实体时逐组件发
`on_destroy`）。现场见 `ecs/render/src/CameraViewSubsystem.cpp` 的 `onBindingDestroyed`。

### 立即观察者 vs 帧内轮询：按「是不是异步就绪」分

- **结构性转换**（资源必须与实体同生共死）→ 立即观察者
- **异步就绪**（回复要等若干帧才回来）→ 观察者只记意图，**帧内轮询**装上结果

`addView` 的回复带**要装回世界的句柄**——必须帧内轮询装上；`removeView` 也有回执
（GenericOkReply），但那只是失败可见性，没有要装的状态——路径仍是「观察者读句柄 →
发出即完，`.then` 里只上报不改世界」。两条路径形状不同的判据是**回执里有没有要装回
世界的东西**，不是「有没有回执」——不要「统一」它们。

同理：资产异步加载的 `ensure*` 路径**保留队列/轮询**，不要改成观察者。组件可能在资产
到位之前就构造了，`on_construct` 只触发一次，会错过「资产后来到了」这个转换。

---

## 构建

- 构建树：`E:/SyncForder/CodeRepos/build/RelWithDebInfo/lux-engine`（Ninja，在 repo 外）
- **必须全量 `target all`**：只编单个目标会"全绿骗人"，ui/editor/gameplay 都依赖渲染层句柄
- **改 `modules/*` 公共头后同步三个 install 前缀**（`E:/SyncForder/CodeRepos/install/`
  下的 `Debug/include`、`RelWithDebInfo/include`、`Android/lux-engine/include`）：
  meta-gen 解析的是**安装前缀**那份头,不同步则被反射组件的 include 读到旧头 ——
  轻则报「未声明」,重则 **EXIT=0 静默降级**(生成带错型的反射)。成因:生成器的
  include 路径钉在 install 树(资产驻留批 1/2/4 三改三同步是现场)。
- **`-j 4` 不能省**：meta-gen 并行度高时会 OOM 并**静默降级**（EXIT=0，但生成带错型的反射）
- `-k 0` 让一次构建吐出全部错误
- 改了 `CMakeLists.txt` 之后要**跑两轮**，第二轮应当 `ninja: no work to do`
- **不得并发**跑构建与实机验证（obj 锁 / 半写 DLL 会造出假崩溃）
- Foundation/closure qualification 必须绑定一个 clean tracked commit；先运行
  `cmake -DLUX_SOURCE_DIR=<repo> -P cmake/ValidateTrackedSnapshot.cmake`，再从该 commit 的独立
  clean clone 配置。不得用被 `.gitignore` 隐藏的本地源码补齐 CMake 输入；仓库根也不得再用裸
  `test` 规则忽略任意层级的 source test 目录。
- 默认验证矩阵不再包含 Android configure/build/CTest/closure；除非用户另行明确要求。
  上述 Android install include 同步仍保留，它只是避免 meta-gen 读取旧公共头，不代表
  Android 构建验证。
