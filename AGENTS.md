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

### 命名
- **类名 / struct（含纯数据类型）**：首字母大写驼峰 `XxxYyyy`。
- **成员变量**：小写下划线 `xxx_yyy`；private 成员加末尾下划线 `xxx_yyy_`。
  成员类型太长时用 `using` 起别名，保持声明在列上对齐。
- **成员函数**：小写驼峰 `aaaBbbb()`。
- **枚举**：一律 `enum class`，类名 E 前缀驼峰 `EXxxYxx`；
  成员全大写下划线 `XXX_YYY_ZZZ`（存量有很多不遵守的，改到就顺手正名）。

### 长语句换行
函数调用过长时逐参数换行、**闭括号放到下一行并保持层级**；参数尽量同一行，
过长先考虑是不是设计问题：

```cpp
xxx::object.invoke(
    aaaa,
    bbbb.make_object(
        dddd
    ),
    cccc
)
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

- Canonical ontology 固定为 `L0 Modules -> L1 World + Simulation -> L2 Process ->
  L3 Scene -> L4 Authoring -> L5 Toolchain -> L6 Product`。World 描述事实；Simulation
  同步解释和运行事实；ECS 只是 Simulation 内部机制；Process 负责异步、IO 与 residency；
  Scene 只组合 World 与 Simulation。不得恢复顶层 `engine/ecs` 或 `ECS` classifier。
- `WORLD` 与 `SIMULATION` 是 L1 sibling roots。纯 `world/core` 只能依赖 L0 primitives；
  `world/asset` 才能增加 L0 Asset/Serialization closure。Simulation 可依赖 World 与 L0，
  不得依赖 Process、Scene、Authoring、Toolchain、Editor 或 Host。旧 `RUNTIME` classifier
  只对白名单存量 extension 暂留；新 L2/L3 target 必须分别使用 `PROCESS/SCENE`。
- 所有 production target 必须调用 `lux_classify_target(TARGET ... LAYER ... PRODUCT ... ROLE ...)`。
  不得靠未分类 target、`INTERFACE_LINK_LIBRARIES` 或 generator expression 绕过 DAG 门禁。
- `LUX_BUILD_PROFILE` 只有 `DEVELOPER/PLAYER/EDITOR/TOOLCHAIN`；Android 使用 `PLAYER`
  配合 Android toolchain/triplet；不得恢复 OS-shaped profile，也不得恢复
  `LUX_BUILD_EDITOR`。同一 Developer 构建树里的 `lux_player` 也必须保持 runtime-clean。
- BUILD_TOOL 关系只用 `lux_add_build_tool_dependency()`、custom command 与 generated file；
  shader compiler、asset packer、meta generator 不得作为 Runtime link dependency。
- 存量 `engine/runtime/execution` 是待迁移的领域盲基础设施，只链接 stdexec、standalone Asio、oneTBB、
  concurrentqueue 与 lux-cxx 基础组件；Asset/Render/Log 是它的 client，方向不得反转。
- Render 固定为 `render_client/render_graph/render_vulkan/render_features` 四 target。
  Simulation extraction 与 FrameCoordinator 只依赖 client；无 render integration pack 的 Scene 必须 headless。
- Authoring 保存可编辑源数据，Toolchain 执行 authoring→cooked，Player 只读取 RuntimeLaunchManifest
  与 cooked pak。Assimp、shaderc、spirv-cross、MLIR/LLVM、Editor UI 不得进入 Player 闭包。
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
