# MA11：唯一 allocation 的代码保活

实现：`e58a17a36c56027fb2d51d6e15144c357eb6080c`。
lux-cxx/依赖 SDK：`bf779515a120350c7c5412c366eea73afdf59ccf`，dirty=false。
**本项通过，关闭 Q-P06-CLANG-GET_DELETER；MA11 与整体整改仍为 PARTIAL。**

## 改动和原失败

原 `pinCodeOwner(CodeLease, shared_ptr<T>)` 接受已经共享的对象，再用 `std::get_deleter`
识别并展开同一代码 owner 的重复包装。真实禁用 RTTI 的 clang-cl/MSVC STL 将该函数定义为 deleted。
生成器补正后，对旧 Engine 实现再次执行完整 Clang all，仍真实失败在该调用；原输出完整保留。

两个活动生产者都是首次创建 allocation：FlowNodeCatalog 和 NativeCallDefinition。
因此将公开入口改为接收 `unique_ptr<T, D>`，在首次共享时建立宿主代码保活桥；后续复制返回的
shared_ptr 即可。删除旧共享输入重载和 get_deleter 展开循环，两个调用点同步迁移。
不通过缓存、RTTI、use_count 或另一套共享外壳识别重复包装。

宿主 CodeOwner 释放桥保持原顺序：释放值及其内部控制块、完成 deleter 清理和返回，再释放代码 pin。
最后 weak 引用只持有宿主控制块。普通值允许在最后强引用所在的线程释放；LuxObject 仍使用
shareOnRuntime 的原线程亲和回收，本次未改变 ObjectRuntime、任务调度、队列或退休算法。

## 实际验证

先验证 clean tracked 快照，再在独立检出构建；复用已有构建树，**不声称本次是冷构建**。
全部正向构建执行 `all -j 4 -- -k 0`，第二轮逐份日志确认无新增工作。

| 范围 | 结果 |
|---|---|
| Editor | 全量构建、无新增工作、171/171 CTest |
| PLAYER | 全量构建、无新增工作、97/97 CTest |
| MSVC ASan | 全量构建、无新增工作、130/130 CTest |
| 全新 SDK | 七组消费者、28 项测试通过 |
| Clang 安装消费者 | C++20/无 RTTI 构建、无新增工作、真实 DLL 两项测试通过 |
| 公共入口负例 | 正确 unique 输入运行通过；shared 输入命中 C2672；恢复后构建及运行通过 |

三个源码配置的测试名称与原记录逐项相同；原断言保留，在 ownership 原测试中新增行为：
真实 DLL 创建普通值和 move-only 状态型 deleter，复制返回的共享 owner，撤销外部库引用，
worker 释放最后强引用；确认对象析构、deleter 返回和清理均先于卸载，最后再销毁 weak 控制块。
同时用公共头静态断言限定输入。安装消费者重跑相同实际实现，没有声明级替身或私有头。

七组消费者为 object-ownership、object-core、flow-native、flow-analysis、flow-scalar、flow-payload、
graph-edit；保留真实 Flow 编译、外部定义/插件、源编码及原内存生命周期断言。
ASan 核验 262 个第一方目标和 617 个实际源码 TU；预编译第三方未插桩，STL size 注解继续按原二进制兼容限制关闭。
这不代表 UBSan、原生输入或 Linux 资格。

新 SDK：`D:/LuxQualification/ma11-code-owner-install`。
改动公共头与独立源码及三个开发 include 前缀逐字节一致；旧副本已保存，Android 仅同步头。

## Clang 剩余限制

实际 Clang/UBSan all 中 CodeLease.cpp 已成功编译，原 get_deleter 错误消失。
全量仍退出 1：25 个失败构建项全部报告 RuntimeLibrary 的 MT/MD 链接冲突；没有运行 UBSan CTest。
没有用独立 Clang 消费者或 MSVC ASan 的通过成绩替代这次失败。

对应的 [LLVM 19.1.5 compiler-rt 配置](https://github.com/llvm/llvm-project/blob/llvmorg-19.1.5/compiler-rt/CMakeLists.txt#L362-L380)
在 MSVC 分支强制静态 CRT，并去掉 MD 选项，与实际失败库的指令一致。
未修改上游 runtime、混入静态 CRT、压制 mismatch 或启用 RTTI。匹配上游源和哈希已归档。

## 归档与保留项

外部归档：`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma11/code-owner-evidence`。
164 个文件、58 条命令；manifest SHA256：
`b2d9b2602e6cb4a708a8ce8f5bddbae09504e44330753e199a85da28083957a1`。
中文/空格路径搬迁通过，缺失及篡改真实 Clang 测试日志均被拒绝，恢复后通过。

六处用户差异逐字节未变，ProjectBuilder 补丁仍未应用；main 与历史记录未修改。
实际图编辑器呈现缺口、Linux 未提供环境、原生输入延期、IME 以及此前已登记失败仍保留。
本次绿色回归不构成此前 transfer_idle、最小化或其他历史问题的根因修复。
