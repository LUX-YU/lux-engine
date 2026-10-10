# MA11：函数间距与 CMake 排版收尾

实现：`43520a08e07bce444e9e719a7e29885b12e46336`。
依赖 lux-cxx：`bf779515a120350c7c5412c366eea73afdf59ccf`，沿用此前已验证的安装前缀。
本次排版补正已完成验证；整个生命周期与机制整改仍为 **PARTIAL**。

## 改动与不变性检查

在本轮终态改造涉及的文件中，修正 12 个 CMake 文件的多行调用、参数分组和调用间隔；
63 个 C++ 文件补入 321 处定义间空行。C++ 只采用 clang-format 给出的纯空白分隔，
没有套用其它格式替换。逐文件验证全部非空行保持不变；CMake 验证参数、引号和括号
token 序列不变，Lua bracket 参数单独处理换行编码。没有修改业务算法、断言或测试准入。

六处用户工作区差异全部按原哈希保留，包括 `modules/function/ui/CMakeLists.txt`；
它们未参与本次提交或独立资格检出，不因本次排版要求擅自修改。ProjectBuilder 补丁仍未应用。

公共头原始字节参与已有插件 SDK ABI 计算，因此空行也改变 SDK 指纹。
按现有算法重算全部 537 个实际公共头，确认旧值
`3532938417f37a4c3bfdf05cf62ab0ce09225c76d1fc753e5418fe415985cef1`
变为 `2c5d9f8b2e191afdbf4bbe699f3fd1b359168610643113fe8545b376f123dd19`。
Editor 的 727 条、PLAYER 的 661 条编译输入仅有相应 SDK ABI 宏变化，其它参数逐项一致。
不放宽插件身份检查，不据此承诺旧插件二进制兼容；相关二进制按新 SDK 重新构建。

## 实际验证

独立 clean tracked 源码：`D:/LuxQualification/ma11-r1-source`，先通过 ValidateTrackedSnapshot。
复用现有构建树，不称为冷构建。构建与 GPU 测试串行；all 均使用 `-j 4 -- -k 0`。

| 项目 | 实际结果 |
|---|---|
| Editor | 全量构建、第二轮无新增工作、171/171 CTest；包括原 40 个 GPU、8 个 desktop 标签测试 |
| PLAYER | 首轮 all 失败；指定对象重编后的 all、无新增工作、97/97 CTest 通过 |
| 新 SDK | 安装到此前不存在的 `D:/LuxQualification/ma11-format-install` |
| Object-only 消费者 | all/无新增工作、2/2；独立公开头编译，未引入 Process/stdexec |
| ObjectScheduler bridge 消费者 | all/无新增工作、1/1；独立公开头编译，未引入 Editor/UI |
| TaskScope 消费者 | all/无新增工作、1/1；保留原非阻塞取消与完成交付断言 |
| 安装头 | 10 个 modules 公共头在新 SDK 及 Debug、RelWithDebInfo、Android 三前缀均与资格源码字节一致 |

Editor/PLAYER 测试名称序列与代码保活补正的原记录逐项一致；测试源只有空行变化，原断言保留。
安装消费者只使用新安装的生产头、库和 DLL；仓库仅提供原测试输入。
Android 仍仅同步头，不表示构建资格。本轮没有重跑 ASan、UBSan、TOOLCHAIN 或人工原生输入。
这些项目仍引用其原实现 SHA 和准确覆盖范围，不将旧成绩登记为本次运行。

## 保留的失败与验证器修正

PLAYER 首轮在链接 `flow_analysis_test.exe` 时实际报 LNK1103：
`lux_engine_function_flowforge.lib(FlowGraph.cpp.obj)` 的调试信息损坏。
保存原完整日志及 obj/lib/pdb 后，只移除明确的构建对象 `FlowGraph.cpp.obj`；
随后的全量构建重新编译该对象并链接相关目标，才执行无新增工作与完整 CTest。
没有改源码、编译选项、调试检查或加入自动重试。首轮失败不改判；缓存调试信息损坏的根因尚未证实。

初次输入比较在 Editor 测试通过后因 SDK ABI 宏变化停止；核验派生依据后，比较器仅识别该
具名变化，并保留完整差异，其他命令参数仍要求精确相同。已完成命令按 SHA 和日志哈希复用，未覆盖。
辅助排版/归档脚本曾因 CRLF 与 Git LF 对比、copyfile 目标误用目录而停止；原脚本、工作字节、
说明和未完成目录保留。这些是辅助工具问题，不归类为生产测试失败，也不伪装成首次全通过。

## 归档与未完成范围

外部归档：
`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma11/format-r1-evidence`。
254 个文件、23 条实际命令；manifest SHA256：
`9e98d3e42a32556637f7151828842f3c3f086190de2100cdada0f65072298b62`。
中文/空格路径搬迁通过；缺失或篡改真实 TaskScope SDK 日志被拒绝，恢复原字节后通过。

Linux 未提供环境、Windows UBSan CRT 冲突、MA08 实际图编辑器呈现仍未满足最终门禁。
原生输入继续延期，IME 未测，旧性能样本不补；transfer_idle、最小化、蒙皮 WAR、
VERTEX_COLOR 的历史失败和责任均不因本次通过而关闭。没有将实例 tint 或常量冒充顶点颜色。
main、历史验收和用户补丁不变。唯一施工状态继续写入 `.internal/editor-redesign/terminal-architecture/`。
