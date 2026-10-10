# MA11：UBSan 失败分类与回调测试补正

实现：`61007eedca8c3feb627ae00feb8b0f13ee0b4b56`。
本次只修正编译负例脚本，不修改生产代码、依赖库或 sanitizer 检查。
**MA11 仍为 PARTIAL，UBSan 总门禁未通过。**

## 一项实际验证缺陷

原 `framework.callback_contract` 在 Clang 下没有执行到正常对照：CMake 的真实编译命令包含
`--`，脚本却在最后追加 `/DLUX_CALLBACK_CASE=0`，Clang 因而将其当成输入文件。
原始失败保留在 `e0faf423e` 的完整 CTest 输出中。

补正将宏插入选项结束符之前；没有结束符时仍追加。保留原输出重定向、真实公共头、
正常回调必须成功、七种可能抛异常的回调必须失败，以及诊断必须命中约束的全部断言。

从独立 clean tracked 检出验证本次提交，复用现有构建树：

- Clang 19.1.5 / UBSan trap 与 MSVC Editor 均执行完整 `all -j 4 -- -k 0` 及第二轮，均无新增工作。
- 两种实际编译器的 `framework.callback_contract` 分别通过；各包含一个正常对照、七个拒绝场景。
- 没有重跑完整 CTest、PLAYER、安装消费者或 GPU。原完整成绩继续绑定原实现 SHA，
  不将单项修后通过改写为新的 `101/130` 全量成绩。
- 无生产接口、安装内容或公共头变化，因此没有新 SDK 或三前缀同步声明。

## 运行时失败的调查范围

原 `e0faf423e` 的 UBSan 完整运行仍为 100/130，30 项失败。本次对其中 27 个直接可执行
测试取得实际首个 trap 栈；另外两个包装测试分别定位到正常子进程和 `apply-in-draw`
子场景的首个 trap。调试器捕获不等于测试通过，更不证明首个失败之后的分支安全。

Object/UI/Render/Process 等多个栈直接显示 `tl::expected` 构造。另对 MaterialNodeCatalog、
FlowNodeCatalog、TaskScope、ScriptSystem 的原编译单元生成报告模式 LLVM IR：对应错误返回
可见 `expected_impl.hpp:1620` 的空基类构造检查。IR 仅用于定位，没有链接诊断运行库或
执行“修后成功”程序；没有修改 expected 的布局或关闭 object-size。

三个插件/场景用例首个 trap 则位于 `wmemcmp+0x301`，经 `std::filesystem::canonical`
调用，不能一并归为 expected 问题。本次使用合法的 wchar_t 数组子范围、长度 4，
通过实际 `wmemcmp` 函数指针建立不依赖 Engine 的对照：普通编译返回比较相等；同一
Clang 工具链的完整 UBSan trap 参数下退出 `0xc000001d`，调试栈同样位于 `wmemcmp+0x301`。
这提供了独立的工具链/运行库边界复现，不代表整个文件 IO 已获 sanitizer 资格。

调查使用的实际测试二进制来自 `e0faf423e`；补正提交只有测试脚本和既有文档变化，
两轮 no-work 也确认没有重编二进制。报告模式 IR 的命令绑定 `61007eed`，原生产 TU 未变化。
UI 调试辅助命令在捕获真实 trap 后，`ub` 无法反向解码而退出失败，原输出保留；
没有将该辅助命令标为成功，也没有借此修改 UI 测试判断。

## 归档与未通过范围

证据位于源码树外：
`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma11/ubsan-classification-evidence`。
共 119 个文件、46 条命令，manifest SHA256：
`a00d0f52e846b45b076a089336b7bd66447146b25b272bb4a9a5c482fe9bd84e`。
中文/空格路径搬迁通过；实际失败日志缺失和篡改均被拒绝，恢复后通过。

本次关闭回调编译探针的选项位置问题，剩余运行时失败没有改判。未压制检查、替换文件算法、
修改第三方头或用 ASan 成绩代替 UBSan。
Linux 无环境、真实图编辑器呈现、原生输入延期、IME 和此前所有历史问题保持原范围。
六处用户差异逐字节保留，ProjectBuilder 补丁未应用；main 与历史快照不变。
