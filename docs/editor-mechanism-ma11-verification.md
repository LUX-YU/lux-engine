# MA11：冷构建、安装闭包与未通过项

实现提交：`7d84e8785ce6778e180d9f78cbb6e96bdd409fb5`。
lux-cxx 源码及依赖 SDK：`0a0e7419fc7229df6e372cd35a540249f92250ef`。
**状态为 PARTIAL。MSVC/ASan 矩阵通过；UBSan、Linux 和实际图编辑器呈现尚未满足最终门禁。**
这份记录不关闭整个机制整改，不覆盖此前失败、免验或延期。

## 本轮实际修正

首次独立冷构建使用 `f17eda8a8e22cbed7a9370aadb44946cbcb942a2`，Editor 和 PLAYER 通过，
TOOLCHAIN 配置真实失败：Scene Render 无条件要求该 profile 不提供的 RenderRuntime/Features。

仅修改 `engine/scene/CMakeLists.txt`：存在真实 `render_runtime` 提供者时才配置 builtin render。
这与既有 Context/plugin 条件一致，不给 TOOLCHAIN 补一个渲染运行库，不增加 mock，不修改测试断言。
新的实际 TOOLCHAIN 图有 265 个目标，保留 Material/Flow 编译器与 CPU Scene/WorldLoading；
不含 render_runtime、render_vulkan、render_features、Editor 或 builtin Scene Render 插件。
Editor/PLAYER 中相应生产目标和测试继续存在。

## 同一修正提交上的运行

从独立 clean tracked 检出 `D:/LuxQualification/ma11-r1-source` 配置。
先执行 ValidateTrackedSnapshot；四个构建树均为新建，未复用旧对象文件。
全部执行 `all -j 4 -- -k 0`，各自第二轮确认 `ninja: no work to do`；构建和实机测试串行。

| 配置 | 实际结果 |
|---|---|
| TOOLCHAIN | 冷构建 572 步、无新增工作、83/83 CTest |
| EDITOR | 冷构建 1366 步、无新增工作、171/171 CTest，包含 40 个 GPU、8 个 desktop 标签用例 |
| PLAYER | 冷构建 1249 步、无新增工作、97/97 CTest；实际 TU 不包含 Editor |
| MSVC ASan | 冷构建 1352 步、无新增工作、130/130 适用 CTest |
| 全新安装 SDK | 26 组消费者、79 项测试通过，每组 all/无新增工作；包括外部 Render 插件实际 GPU 运行 |
| 公共头 | 60 个改动安装头各自独立 C++20/无 RTTI 编译，链接其实际提供组件 |

Editor/PLAYER 的测试名称分别与来源适配阶段的 171/97 项逐项一致；数量不是断言保留的替代证明。
除上述 CMake 条件外，生产与测试代码和 `9de3a098cb71edcbdecdf21e49422477f35b2cb6` 一致。

新安装前缀为 `D:/LuxQualification/ma11-r1-install`。消费者覆盖 Framework、Object、任务、
RuntimeObject、错误、日志、Context 扩展、Render kernel、Material/Flow、GraphEdit、UI、Meta、
Window、Physics2D、脚本、空间数据及外部插件。Material/Flow 的可选真实编译器路径明确开启。
实际 compile_commands、Ninja 链接与安装配置核验未借用源码生产私有头、旧构建 DLL 或 legacy。
全部改动公共头与 clean tracked 源码逐字节一致；51 个 modules 头在三个开发 include 前缀同步。
首次发现 9 个头的 27 份开发安装副本仅有 CRLF/LF 差异，先保存原字节，再同步；Android 仅同步头。
14 个已删除逻辑公开头在新 SDK 和三个开发前缀均不存在。

ASan 使用 262 个实际第一方目标清单；617 个源码 TU 的命令核对到地址插桩。
预编译第三方内部未插桩，STL string/vector size-capacity 注解按既有二进制兼容合同统一关闭。
不关闭普通地址/UAF 检查，不将该结果称作 GPU、Linux 或 UBSan 资格。

## 语义、负例与生成

- Scalar 28,393 字节、async 4,931 字节、control 5,820 字节产物与原始基线逐字节一致；
  module/compiler 两条路径保留原 12 行诊断，实际嵌套执行和读写 1→7→7 通过。
- 四项旧 Menu API、四项旧 Flow source API 均在真实安装头编译中被拒绝；
  每次移除非法调用后恢复构建并执行，不以声明级伪探针代替。
- 实际 File API 分别注入 Object→Process、object_execution→Editor/UI 非法边，
  完成“合法通过—指定规则拒绝—去边恢复”。未修改检查规则来容忍违规边。
- 9 项针对性源码门禁检查 1,291 个 tracked 活动 C++ 文件，覆盖删除机制及权威归属。
  这是定向检查与行为测试的组合，不宣称词法脚本证明所有 C++ 语义。
- 删除安装消费者的两份 Meta 输出和源码构建的 90 份 Render 通信 CPP，再从真实生成器重建；
  输出哈希一致，后续定向测试通过，第二轮无新增工作。原完整 CTest 日志在此之前独立保存。

## 当前未通过项

Clang 19.1.5/UBSan 的独立 clean 配置成功，实际 `all -j 4 -- -k 0` 失败；没有运行 UBSan CTest。
保留完整输出及以下不同责任，不能合并为“环境问题”或使用 MSVC ASan 成绩替代：

| 项目 | 实际证据与责任 |
|---|---|
| UBSan CRT | 随编译器提供的 standalone C++ runtime 为 MT_StaticRelease，工程为 MD_DynamicRelease；lld-link 拒绝。未屏蔽 failifmismatch，未混用 CRT。 |
| Q-P06-CLANG-GET_DELETER | 原 CodeLease 的 `std::get_deleter<CodeOwner>` 被当前无 RTTI MS STL 删除。原限制再次复现；未启用 RTTI、削弱代码 pin 或删除去重复包装语义。 |
| MA11-CLANG-META-IMSVC | 实际 clang-cl 编译命令用 `-imsvc` 提供安装依赖；当前 lux-cxx GeneratorHelper 只收取 -I/-isystem/-external:I。真实 Meta/Render 生成报依赖头缺失。Marker.hpp 在安装前缀实际存在，相关命令、任务与解析器源均保存。此路径尚未修正或资格化。 |
| Linux | 用户暂不提供环境，NOT_RUN/未满足；不配置 WSL，不用 Windows 代替。 |
| MA08 图编辑器 | 当前正式产品没有图编辑器，实际呈现资格未完成；范围决定待定，普通 UI/GPU 与 CPU 图测试不替代该项。 |

既有 `render.transfer_idle` 缺完成标记、`Q-LR03-HOST-MINIMIZE`、蒙皮跨帧 WAR 责任仍保留。
Material VERTEX_COLOR 的原后端失败保持原判断；新旧结果一致不代表支持成功。
原生输入仍 NOT_RUN_USER_DEFERRED，系统 IME 未测，旧 50k 性能样本不补。

## 归档、工作区与提交边界

外部证据：`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma11/evidence`。
5,818 个文件、209 条真实命令，保留首次配置失败与最终修正提交的不同身份。
manifest SHA256：`aeb2e53a43613362c6b97b44c7f39224a2ace5d3197d4690df33e0c9fdf58ead`。
归档验证依赖相对文件和锚定哈希，不依赖生产路径存在。完整中文/空格路径搬迁、真实日志缺失/篡改、
manifest 锚定拒绝及恢复均通过；独立输出见同级
`archive-portability-results.json`。

开发仍在 `E:/SyncForder/CodeRepos/lux-engine`、`codex/editor-framework-v2`。
六处用户工作区差异哈希不变、未纳入本次 clean qualification 或提交；ProjectBuilder 独立补丁未应用。
main、历史分支与既有验收快照未修改。本记录独立于实现提交，施工状态仅维护原 `.internal` 账本。
