# P10Q 来源、调查边界与待验证事项

**日期：2026-09-30。本文只描述本次文档编制读取了什么，不是引擎验收报告。**

## 1. 固定代码版本

| 仓库 | 本次读取的固定提交 | 用途 |
|---|---|---|
| LUX-YU/lux-engine | `b583e7ffe20e7a1ac55c7119d6a13ac337ebb323` | 当前分支 HEAD，P10 R1 验收记录；新阶段参考入口 |
| LUX-YU/lux-engine | `514bca1f180e91a88545d26d5d11a63fcd7e7498` | P10 R1 记录指定的实现；本次没有完整复算该轮资格 |
| LUX-YU/lux-engine | `b9b856477755a9247a7a6810fa880ae62151f565` | 上一轮基础设施/抽象审阅基准；历史参考文件保持原样 |
| LUX-YU/lux-cxx | `bc1eab34b83b5cf8821d6319e5b2a02574dc91fd` | 本次读取的 main 与代表性实际 API |

通过 GitHub 连接读到分支 ref 与提交后，才更新 P10Q 的起点。本次发现新 P10 R1 已交付，不能继续要求“从零实施旧 R1”，也不能未经独立核验自动声明其全部通过。

## 2. 证据等级

| 等级 | 本次含义 | 不能推导 |
|---|---|---|
| 实际源码读取 | 当前或明确固定的历史文件、函数体、CMake 与公开头 | 全仓逐行审阅、所有调用组合正确 |
| 实施方归档 | README/FILES/提交中的测试与故障记录 | 本文编制环境独立运行了同一矩阵 |
| 源码复杂度推断 | 外层每节点循环与内层祖先链可产生深链二次工作量 | 具体毫秒、当前用户机器的热点排名 |
| 设计要求 | P10Q 的新类型、文件去向、规则、测试输入 | 这些类型已存在或已经有性能收益 |
| 盘点脚本自检 | 附带脚本在自建小型 Git 夹具上的解析/输出验证 | 用户仓库的文件数量、DLL 数、实际依赖与任何产品测试通过 |

没有独立构建 lux-engine/lux-cxx，没有执行真实 SDK/GPU/IME/Windows/Linux 引擎测试，没有生成全仓 AST 或对用户项目做自动修改。容器环境无法直接取得完整远端检出，因此没有给出全仓文件长短、DLL 或实际复杂度计数；这类结果由 Q0/Q6/Q7 在实施环境产生。

## 3. lux-engine 主要来源索引

表中“历史”指前次已读且随本次材料提供的固定源码/记录；当前 P10 R1 只修改指定工具 UI 和测试，不意味着我们本轮把每一份历史源码都重新取回。实施者在 Q0 对最新实际工作树复核。

| 编号 | 固定来源 | 本文使用的事实与限制 |
|---|---|---|
| E01 | [P10 R1 验收提交](https://github.com/LUX-YU/lux-engine/commit/b583e7ffe20e7a1ac55c7119d6a13ac337ebb323)；分支 ref 的读取值已冻结于第1节 | 远端已经有新交付；不是继续施工旧 P10 HEAD |
| E02 | [提交中的 FILES](https://github.com/LUX-YU/lux-engine/blob/b583e7ffe20e7a1ac55c7119d6a13ac337ebb323/dev_log/P10-R1/FILES.md) | 记录前置 b9b…→实现514…，一份新增测试与六份修改；开始施工时核对 receipt 和实际祖先 |
| E03 | [P10 R1 README](https://github.com/LUX-YU/lux-engine/blob/b583e7ffe20e7a1ac55c7119d6a13ac337ebb323/dev_log/P10-R1/README.md) | 值+based_on、原队列续行、四个修复前真实负例及访问拒绝故障报告来自交付方；本轮未完整复跑 |
| E04 | [LuxObject 基础契约（前次固定源码）](https://github.com/LUX-YU/lux-engine/blob/b9b856477755a9247a7a6810fa880ae62151f565/modules/core/object/README.md) | 信号void/noexcept、Connection、接收者与lambda生命周期、回调栈限制 |
| E05 | [Process 契约（前次固定源码）](https://github.com/LUX-YU/lux-engine/blob/b9b856477755a9247a7a6810fa880ae62151f565/engine/process/README.md)；[ExecutionRuntime.hpp](https://github.com/LUX-YU/lux-engine/blob/b9b856477755a9247a7a6810fa880ae62151f565/engine/process/execution/include/lux/engine/process/ExecutionRuntime.hpp) | 统一 Task/TaskScope、CPU/blocking、完成/业务分发区分、单 TaskObserver |
| E06 | [GraphCanvas.cpp](https://github.com/LUX-YU/lux-engine/blob/b9b856477755a9247a7a6810fa880ae62151f565/editor/widgets/src/GraphCanvas.cpp)；[NodeCanvas.cpp](https://github.com/LUX-YU/lux-engine/blob/b9b856477755a9247a7a6810fa880ae62151f565/editor/widgets/src/NodeCanvas.cpp)；[bootstrap manifest](https://github.com/LUX-YU/lux-engine/blob/b9b856477755a9247a7a6810fa880ae62151f565/bootstrap/lux-manifest.json) | Lux signal 与 ax::NodeEditor 的实际调用；依赖指向用户 fork，revision 为空不是二进制溯源 |
| E07 | [ProjectCatalogAccess.hpp](https://github.com/LUX-YU/lux-engine/blob/b9b856477755a9247a7a6810fa880ae62151f565/editor/project/ui/include/lux/engine/editor/project/ProjectCatalogAccess.hpp)；[MaterialView.hpp](https://github.com/LUX-YU/lux-engine/blob/b9b856477755a9247a7a6810fa880ae62151f565/editor/tools/material/ui/include/lux/engine/editor/material/MaterialView.hpp)；[TaskQueryPort.hpp](https://github.com/LUX-YU/lux-engine/blob/b9b856477755a9247a7a6810fa880ae62151f565/editor/tasks/ui/include/lux/engine/editor/tasks/TaskQueryPort.hpp) | 指定手工函数表、action/错误域、任务转发与可选revision；设计不足不等于本次已复现UAF |
| E08 | [ProjectStorage.hpp（本次读取）](https://github.com/LUX-YU/lux-engine/blob/b583e7ffe20e7a1ac55c7119d6a13ac337ebb323/editor/storage/include/lux/engine/editor/storage/ProjectStorage.hpp) | 原数组/by-id/revision与两个信号存在；提取 CatalogModel 是本方案要求，不是现状 |
| E09 | [SaveExecution.cpp](https://github.com/LUX-YU/lux-engine/blob/b9b856477755a9247a7a6810fa880ae62151f565/editor/adapters/project_io/src/SaveExecution.cpp)；[ProjectArtifactStore.cpp](https://github.com/LUX-YU/lux-engine/blob/b9b856477755a9247a7a6810fa880ae62151f565/editor/adapters/project_io/src/ProjectArtifactStore.cpp) | adapters 下有正式执行/文件副作用，不是可直接丢掉的旧壳；改归属不改发布语义 |
| E10 | [Scene UI CMake（本次读取）](https://github.com/LUX-YU/lux-engine/blob/b583e7ffe20e7a1ac55c7119d6a13ac337ebb323/editor/tools/scene/ui/CMakeLists.txt)；[Editor CMake](https://github.com/LUX-YU/lux-engine/blob/b583e7ffe20e7a1ac55c7119d6a13ac337ebb323/editor/CMakeLists.txt) | native 条件下强制 lld-link 和 GPU/综合程序；实际测试能力耦合，不冒称所有Linux生产目标都已失败 |
| E11 | [SceneSource.cpp（本次读取）](https://github.com/LUX-YU/lux-engine/blob/b583e7ffe20e7a1ac55c7119d6a13ac337ebb323/editor/tools/scene/model/src/SceneSource.cpp) | validate 外层逐对象调用祖先校验；对象/组件复制与预算的具体入口 |
| E12 | [SceneAlgorithms.hpp（本次读取）](https://github.com/LUX-YU/lux-engine/blob/b583e7ffe20e7a1ac55c7119d6a13ac337ebb323/editor/tools/scene/model/include/lux/engine/editor/scene/SceneAlgorithms.hpp) | createsParentCycle 实际while父链，因此可推断深链全图校验O(N²)；未计时 |
| E13 | [TreeRows.cpp（本次读取）](https://github.com/LUX-YU/lux-engine/blob/b583e7ffe20e7a1ac55c7119d6a13ac337ebb323/editor/widgets/src/TreeRows.cpp)；[OutlinerView.cpp](https://github.com/LUX-YU/lux-engine/blob/b583e7ffe20e7a1ac55c7119d6a13ac337ebb323/editor/tools/scene/ui/src/OutlinerView.cpp) | 已有first/next和显式栈、UUID索引；不将所有for循环笼统认定二次复杂度 |
| E14 | [NodeCanvasIds.hpp](https://github.com/LUX-YU/lux-engine/blob/b9b856477755a9247a7a6810fa880ae62151f565/editor/widgets/include/lux/engine/editor/widgets/NodeCanvasIds.hpp) | 3个映射与originals增长；长期churn的回收/上下文重建需正式资格，不能无条件复用旧widget ID |
| E15 | [persistence CMake（本次读取）](https://github.com/LUX-YU/lux-engine/blob/b583e7ffe20e7a1ac55c7119d6a13ac337ebb323/editor/persistence/CMakeLists.txt) | 现有STATIC纯协调器及独立consumer；允许同主题保留必要的执行target而非合成巨库 |
| E16 | [ViewportPresentation.hpp（本次读取）](https://github.com/LUX-YU/lux-engine/blob/b583e7ffe20e7a1ac55c7119d6a13ac337ebb323/editor/tools/scene/projection/include/lux/engine/editor/scene/ViewportPresentation.hpp)；[Material UI CMake](https://github.com/LUX-YU/lux-engine/blob/b9b856477755a9247a7a6810fa880ae62151f565/editor/tools/material/ui/CMakeLists.txt) | 视口依赖实际不含作者模型；Material目前PUBLIC链接scene_ui。共享viewport迁移为方案 |
| E17 | [P07 README](https://github.com/LUX-YU/lux-engine/blob/b9b856477755a9247a7a6810fa880ae62151f565/dev_log/P07/README.md)；[P10 README](https://github.com/LUX-YU/lux-engine/blob/b9b856477755a9247a7a6810fa880ae62151f565/dev_log/P10/README.md) | 新模块已STATIC；整投影快照成本已有披露；本阶段不虚构“删除许多新增DLL”的成绩 |

补充已读历史文件：Material/Flow ViewFactory 的关闭失败转换；两种 Publish* CPP/header 的静态 Operation 与重复准入算法；InspectorFields 的即时 std::function 参数和长期 structural_；Material/Flow 编译对 Process 的实际调用。完整旧审阅索引在 `reference/INFRASTRUCTURE_REVIEW.md`，其日期和代码基准不改写。

## 4. lux-cxx：哪些 API 读到了，哪些还需资格

所有下列链接固定为 `bc1eab34b83b5cf8821d6319e5b2a02574dc91fd`。

| 编号 | 实际来源 | 可支持的结论与使用限制 |
|---|---|---|
| L01 | [README.md](https://github.com/LUX-YU/lux-cxx/blob/bc1eab34b83b5cf8821d6319e5b2a02574dc91fd/README.md) | 有core/container/memory等现有设施；expected C++20入口保留。README性能措辞不作为引擎性能证据 |
| L02 | [StableSlotMap.hpp](https://github.com/LUX-YU/lux-cxx/blob/bc1eab34b83b5cf8821d6319e5b2a02574dc91fd/container/include/lux/cxx/container/StableSlotMap.hpp) | 本次读取前245行：块存储、默认256、SlotKey、reserve/emplace/erase/clear与generation；无跨Store域、无自动callback保护。SlotMap本体需要另行资格 |
| L03 | [SparseSet.hpp](https://github.com/LUX-YU/lux-cxx/blob/bc1eab34b83b5cf8821d6319e5b2a02574dc91fd/container/include/lux/cxx/container/SparseSet.hpp)；[container README](https://github.com/LUX-YU/lux-cxx/blob/bc1eab34b83b5cf8821d6319e5b2a02574dc91fd/container/README.md) | 前185行表明key-offset sparse数组和dense记录；自动分号复用描述来自README，替换前验证实际路径。不能把UUID/大hash直接作下标 |
| L04 | [SmallVector.hpp](https://github.com/LUX-YU/lux-cxx/blob/bc1eab34b83b5cf8821d6319e5b2a02574dc91fd/container/include/lux/cxx/container/SmallVector.hpp) | 本次读取前210行：N内联、allocator、对齐和relocation实现；没有跑全部move/insert/allocator测试。旧注释对std::vector失效规则的泛化不应照搬 |
| L05 | [function_ref.hpp](https://github.com/LUX-YU/lux-cxx/blob/bc1eab34b83b5cf8821d6319e5b2a02574dc91fd/core/include/lux/cxx/core/function_ref.hpp) | 已读完整文件；非拥有，lvalue callable构造，普通签名特化，不假设支持任意noexcept签名/临时绑定 |
| L06 | [Delegate.hpp](https://github.com/LUX-YU/lux-cxx/blob/bc1eab34b83b5cf8821d6319e5b2a02574dc91fd/core/include/lux/cxx/core/Delegate.hpp) | 已读完整文件；typed bind、可空、非拥有；不替代LuxObject的连接寿命 |
| L07 | [move_only_function.hpp](https://github.com/LUX-YU/lux-cxx/blob/bc1eab34b83b5cf8821d6319e5b2a02574dc91fd/core/include/lux/cxx/core/move_only_function.hpp) | 本次读前130行：32B及alignment/nothrow-move条件，拥有/移动与reset；不是所有闭包零分配或noexcept调用保证 |
| L08 | [core头目录](https://github.com/LUX-YU/lux-cxx/tree/bc1eab34b83b5cf8821d6319e5b2a02574dc91fd/core/include/lux/cxx/core) | 核实scope_exit、CheckedArithmetic、StrongId等文件存在；具体使用前继续读实现和测试，不在文档杜撰函数名 |
| L09 | [PmrResources.hpp](https://github.com/LUX-YU/lux-cxx/blob/bc1eab34b83b5cf8821d6319e5b2a02574dc91fd/memory/include/lux/cxx/memory/PmrResources.hpp) | 本次读前210行：Counting/Budget/FailingMemoryResource；计量只覆盖经其分配的数据，不是全进程或GPU |
| L10 | [container头目录](https://github.com/LUX-YU/lux-cxx/tree/bc1eab34b83b5cf8821d6319e5b2a02574dc91fd/container/include/lux/cxx/container)；[memory头目录](https://github.com/LUX-YU/lux-cxx/tree/bc1eab34b83b5cf8821d6319e5b2a02574dc91fd/memory/include/lux/cxx/memory) | 核实SlotMap/HeterogeneousLookup/SharedBytes等存在；并非每份完整实现均审阅。实际安装版本仍需Q0固定 |

本次还发现文档与头文件存在不同步：memory README仍写planned，而目录已有PmrResources/SharedBytes；container README部分叙述落后于allocator-aware头；README指向的一份迁移文档返回404。因此，不按README的“更快/无额外成本/跨平台”概述直接作替换与资格结论。

## 5. 外部 C++/构建规范来源

以下仅支撑通用规范，不能覆盖实际仓库证据或作为本项目测试结果。

| 编号 | 一手/官方来源 | 本阶段采用的内容 |
|---|---|---|
| W01 | [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines) | 用类型/资源管理表达责任，标准模式和实际成本；不是要求机械套用全部规则 |
| W02 | [CMake add_library](https://cmake.org/cmake/help/latest/command/add_library.html) | STATIC、SHARED、MODULE、OBJECT、INTERFACE是不同构建/装载责任 |
| W03 | [CMake POSITION_INDEPENDENT_CODE](https://cmake.org/cmake/help/latest/prop_tgt/POSITION_INDEPENDENT_CODE.html) | 将静态对象用于需要位置无关代码的目标时，按target配置而非跨平台硬塞编译flag |
| W04 | [CMake CXX_EXTENSIONS](https://cmake.org/cmake/help/latest/prop_tgt/CXX_EXTENSIONS.html)；[target_compile_features](https://cmake.org/cmake/help/latest/command/target_compile_features.html) | 真实声明语言需求与扩展策略；还要实际检查最终编译命令和编译器模式 |

不要求更新到链接页面的最新 CMake 版本；沿用项目声明支持的版本，使用该版本实际支持的能力。第三方库README中的微基准数值不作为BQ的任何预期数值。

## 6. 实施者在 Q0 必须补充的调查

当前提供的是有具体起点的实施方案，不是伪装成已完成的全量审计。Q0至少补齐：

| 缺失事实 | 实际取得方式 |
|---|---|
| 当前生产/测试/生成/历史源的完整集合与行数 | 固定Git对象及当前工作区差异，附带脚本+人工分类 |
| 所有Editor动态库和加载消费者 | CMake File API、链接命令、运行装载/插件路径，而非源码grep |
| 每个移除检查的有效期 | AST/调用链/回调与异步边界人工核对 |
| 选用lux-cxx API的正确性和收益 | 固定真实已安装版本的组件测试、实际payload与计量 |
| 目录/任务稳态复制和层级校验实际成本 | 本阶段BQ输入、算法计数、分配/复制及同机样本 |
| Linux完整生产目标与适用测试 | 新检出、新build tree和准确依赖；未跑就不写PASS |
| Workspace访问拒绝的来源 | 真实失败现场、句柄/并发/fixture隔离和精确重跑；不简单忽略 |

如新增源码反证推翻某条假设，应更新现行处置表，保留旧审阅作为历史。不以“文档写过了”强行制造失败或修改本来正确的算法。
