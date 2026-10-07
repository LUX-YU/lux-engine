# lux-engine 工程约定

本文件的主体记录**可机检、会反复被违反**的结构规矩。每一条都来自一次真实的返工，
后面附着它的成因——不写成因的规矩会在下一次"就这一次"里被绕过。
前两节（项目簇背景、代码风格）是用户定的基线约定，随仓走。

（早期设计材料曾按用户要求清理。后续阶段的唯一可变施工材料位于
`.internal/editor-redesign/`，验收输出保存于源码树外，按各自 implementation SHA 核验。
历史 dev_log 已经逐文件验证归档，Git 历史保持不变；不修改过去的判定，用户免验或未测不能标成通过。）

---

## 项目簇背景（lux-\*，同级文件夹）

lux 是一个项目簇；lux-engine 是其中的游戏引擎——`modules/` 提供可被外部项目
复用的基础应用功能，`engine/` 提供游戏与编辑器共用的引擎功能，顶层 `editor/` 提供编辑器产品。
依赖方向为 `editor -> engine -> modules`，Editor 可以直接使用 modules，底层不得反向链接 Editor。
Editor Framework v2 按 app/window/context/project/ui 组织，具体工具以后采用纵向目录。
editor_legacy 是冻结参考源码，不参与当前配置、构建和安装，不得成为正式路径的依赖。
ObjectRuntime 统一对象线程与代际身份；LuxObject parent 只表达结构，不决定删除。
ui::Root 的 SlotKeyAutoSparseSet 唯一拥有全部顶层 Pane；Pane 只有一个内容根，不包含子 Pane。
本轮不接入旧服务容器、命令目录、写入协议或具体工具。通用基础的现有消费者不因此删除。
编译与执行入口见
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
- 相邻函数定义之间保留一个空行；不得把一个函数的结束括号与下一函数定义贴在一起。
- 多行 CMake 调用按参数语义分组，结束 `)` 独占一行；相邻配置调用之间保留空行。
- 单条语句在不超过 120 列时保持一行；只有超过 120 列或层次明显影响可读性时才换行。
- 换行应表达语义层次，不要为了凑列宽把一个完整表达式拆成难以阅读的碎片。
- 多行列表、初始化和调用的结束括号/花括号独占下一行，并与对应的开始层级对齐。

### 命名
- **类名 / struct（含纯数据类型）**：首字母大写驼峰 `XxxYyyy`。
- **variant 类型别名**：使用 `V` 前缀，例如 `using VSimulationClock = std::variant<FixedStepClock>;`。
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

如果参数列表仍超过 120 列，再按参数逐行换行，注意换行后的参数仅仅比函数名多一级缩进，最后的括号也需要单独换行：

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

### 缩进与对齐
所有的代码需要按照所在的层级缩进，缩进为4个空格。
```cpp
{
    {
        {

        }
    }
}
```
注意在类声明中，public/protected/private和括号同级。

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

### 异常
该项目一般情况下不能使用异常。

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

### 普通堆内存耗尽不可恢复

普通 new/STL 分配的 OOM 为 fatal；expected 不承诺恢复 bad_alloc，不声明没有真实可恢复生产者的
ALLOCATION_FAILURE。边界已有异常转换时先终止 bad_alloc，再转换其它外部异常；不新增热路径 catch。
显式容量耗尽、设备分配和外部后端的失败仍保留准确错误，不与普通堆 OOM 混为一谈。
错误描述在模块装配时登记，失败点只构造预声明 ErrorId 和数值参数，不在 failure path 注册字符串。

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

## Editor 持续质量规范

P10Q 及后续 Editor 修改遵守 [docs/editor-quality.md](docs/editor-quality.md) 的 QR01–QR22。
该文件只定义持续规则；唯一可变施工材料仍为 `.internal/editor-redesign/`，不另建状态账本。
Framework v2 不沿用历史迁移阶段门禁；历史规则只在其原 Git 提交核验。
新框架核验真实 source/include/link/install 闭包，不允许 legacy 或旧 SDK 残留补全依赖。
