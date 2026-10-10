# MA11：Clang 链接补正与 UBSan trap 实测

生产修正：`a213be826`；测试夹具补正及最终实现：`e0faf423e1e7fbc3e160e1341d891891d24970fc`。
lux-cxx 和依赖 SDK 保持 `bf779515a120350c7c5412c366eea73afdf59ccf`，没有修改依赖库。
**两项构建问题已修正；UBSan 与 MA11 整体仍未通过，状态 PARTIAL。**

## 实际修正

原诊断运行库的 MT/MD 冲突阻止了 Clang 完整链接。本次使用
[Clang 19 官方支持的 trap 模式](https://releases.llvm.org/19.1.0/tools/clang/docs/UndefinedBehaviorSanitizer.html)，
保留 `-fsanitize=undefined`，增加 `-fsanitize-trap=all`，无需链接该运行库。
旧运行库失败仍为 FAIL，没有修改 runtime、压制 mismatch 或关闭检查。

先核对实际编译器展开的 19 项检查完全一致，再运行溢出、移位、对齐、bool 四个真实负例。
负例均以非法指令终止，正常输入通过；IR 含 `llvm.ubsantrap`，不含运行库 handler。
这些只是插桩有效性对照，不代替实际工程测试。

首次对 `43520a08` 执行完整 Clang 构建，出现四个失败项，原输出保留：

- `GeneralRenderServer::Impl` 公开前置声明为 class，实际定义为 struct。真实 DLL 导入库使用
  struct 修饰符，RendererThread 对象文件却请求 class 修饰符的两个符号。只将前置声明改为
  struct；修后双方符号一致，RenderRuntime 正常链接。不修改 owner、构造或退休算法。
- 三个渲染故障测试共用的 `shadow_native.hpp` 在开启宏拦截后才读取 VMA 前置声明，
  导致声明被替换成已有 C++ 测试函数名，但仍带 C 语言链接。将同一个正式前置声明头提前读取，
  保留全部故障注入、实际 provider 源码及原断言，不修改 VMA 或生产接口。

原导入库、消费方对象文件和修前/修后符号输出均归档。两个修改分别提交。

## 最终实现上的验证

先检查 clean tracked snapshot，再在独立检出配置。复用已有构建目录，不声称冷构建。
构建和测试串行，完整构建均为 `all -j 4 -- -k 0`，第二轮无新增工作。

| 范围 | 实际结果 |
|---|---|
| Clang 19.1.5 + UBSan trap | 全量构建、无新增工作通过；CTest **100/130，30 项失败** |
| MSVC Editor | 全量构建、无新增工作、171/171 CTest；含 40 个 GPU、8 个 desktop 标签用例 |
| MSVC PLAYER | 全量构建、无新增工作、97/97 CTest |
| 全新 MSVC SDK | Render kernel/真实插件两项、UI composition 一项通过 |
| 改动安装头 | MSVC 与 Clang 分别独立 C++20、无 RTTI 编译通过，第二轮无新增工作 |

三组测试名称与各自原清单逐项一致；本次没有删除、缩减或修改已有断言。
SDK 位于 `D:/LuxQualification/ma11-ubsan-trap-install`，是 **MSVC 安装产物**，不是插桩 SDK。
改动公共头与新 SDK、Debug/RelWithDebInfo/Android 三个开发 include 前缀逐字节一致；
Android 仅同步头。公共头变化使插件 SDK 身份重新派生，插件已重建，不能混用旧二进制。

## UBSan 未通过的具体证据

262 个第一方目标、617 个源码和 103 个生成编译单元带检查参数。
预编译第三方内部未插桩；原无 RTTI 合同下两种模式均无 vptr 检查。
trap 模式失败时没有诊断运行库的格式化报告，因此补充了实际调试器和原 TU 的 LLVM IR。

`object.ownership` 的实际 trap 位于 `tl::expected` 错误构造中的空基类检查。
使用安装头独立构造 `expected<void, uint8_t 枚举>` 的错误值也复现：普通控制运行通过，
完整 UBSan trap 参数下失败。原 TU 的报告模式 IR 将触发点定位到 `expected_impl.hpp:1620`。
这与 [LLVM 的 Windows 空基类报告](https://github.com/llvm/llvm-project/issues/31914)
及其 [UBSan 构建讨论](https://reviews.llvm.org/D151511) 一致，属于当前诊断依据。
**没有据此认定全部 30 项失败都是同一原因，也没有把它们改判为通过。**

没有禁用 object-size 或其他检查，没有给 expected 添加 ABI/layout 绕过，未更换标准或升级编译器。
完整失败名单、实际输出、调试栈、IR、安装头复现及上游参考分别保存。
最初资格脚本错误套用 MSVC 的 SDK 哈希而停止，也保留原脚本；随后根据 Clang 环境与
537 个物理公共头重算，仅接受真实派生哈希差异，没有放宽生产 ABI 门禁。

## 归档与保留范围

外部归档：`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma11/ubsan-trap-evidence`。
138 个文件、50 条命令，manifest SHA256：
`44116f2ab1b9ff0c5d020723d1292a3f0b881f68d9fba316a4499d627dfb4e1b`。
中文/空格路径搬迁通过；实际 UBSan 失败日志缺失、篡改均被拒绝，恢复后通过。

六处用户差异精确保留；ProjectBuilder 补丁未应用，main 与历史快照不变。
Linux 未提供环境、实际图编辑器呈现、原生输入延期、IME 和原有问题继续保留各自范围。
未重跑 ASan、旧性能长测等未修改路径的资格，不将其旧成绩登记为本次结果。
当前 Windows 绿色回归不关闭之前 transfer_idle、最小化、PLAYER 缓存损坏等未查明问题。
